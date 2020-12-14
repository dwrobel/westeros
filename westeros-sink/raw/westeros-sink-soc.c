/*
 * Copyright (C) 2020 RDK Management
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>

#ifdef USE_GST_ALLOCATORS
#include <gst/allocators/gstdmabuf.h>
#endif

#include "westeros-sink.h"

#define DEFAULT_VIDEO_SERVER "video"

#ifdef GLIB_VERSION_2_32
  #define LOCK_SOC( sink ) g_mutex_lock( &((sink)->soc.mutex) );
  #define UNLOCK_SOC( sink ) g_mutex_unlock( &((sink)->soc.mutex) );
#else
  #define LOCK_SOC( sink ) g_mutex_lock( (sink)->soc.mutex );
  #define UNLOCK_SOC( sink ) g_mutex_unlock( (sink)->soc.mutex );
#endif

GST_DEBUG_CATEGORY_EXTERN (gst_westeros_sink_debug);
#define GST_CAT_DEFAULT gst_westeros_sink_debug

#define INT_FRAME(FORMAT, ...)      frameLog( "FRAME: " FORMAT "\n", __VA_ARGS__)
#define FRAME(...)                  INT_FRAME(__VA_ARGS__, "")

enum
{
  PROP_DEVICE= PROP_SOC_BASE,
  PROP_FORCE_ASPECT_RATIO,
  PROP_ENABLE_TEXTURE
};
enum
{
   SIGNAL_FIRSTFRAME,
   SIGNAL_NEWTEXTURE,
   SIGNAL_TIMECODE,
   MAX_SIGNAL
};

static bool g_frameDebug= false;
static guint g_signals[MAX_SIGNAL]= {0};

static void wstSinkSocStopVideo( GstWesterosSink *sink );
static void wstGetVideoBounds( GstWesterosSink *sink, int *x, int *y, int *w, int *h );
static WstVideoClientConnection *wstCreateVideoClientConnection( GstWesterosSink *sink, const char *name );
static void wstDestroyVideoClientConnection( WstVideoClientConnection *conn );
static void wstSendHideVideoClientConnection( WstVideoClientConnection *conn, bool hide );
static void wstSendSessionInfoVideoClientConnection( WstVideoClientConnection *conn );
static void wstSetSessionInfo( GstWesterosSink *sink );
static void wstSendFlushVideoClientConnection( WstVideoClientConnection *conn );
static void wstProcessMessagesVideoClientConnection( WstVideoClientConnection *conn );
static bool wstSendFrameVideoClientConnection( WstVideoClientConnection *conn, int buffIndex );
static gpointer wstDispatchThread(gpointer data);
static gpointer wstEOSDetectionThread(gpointer data);
static gpointer wstFirstFrameThread(gpointer data);
static bool drmInit( GstWesterosSink *sink );
static void drmTerm( GstWesterosSink *sink );
static bool drmAllocBuffer( GstWesterosSink *sink, int buffIndex, int width, int height );
static void drmFreeBuffer( GstWesterosSink *sink, int buffIndex );
static void drmLockBuffer( GstWesterosSink *sink, int buffIndex );
static bool drmUnlockBuffer( GstWesterosSink *sink, int buffIndex );
static bool drmUnlockAllBuffers( GstWesterosSink *sink );
static WstDrmBuffer *drmGetBuffer( GstWesterosSink *sink, int width, int height );
static void drmReleaseBuffer( GstWesterosSink *sink, int buffIndex );

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

static gint64 getGstClockTime( GstWesterosSink *sink )
{
   gint64 time= 0;
   GstElement *element= GST_ELEMENT(sink);
   GstClock *clock= GST_ELEMENT_CLOCK(element);
   if ( clock )
   {
      time= gst_clock_get_time(clock);
   }
   return time;
}

static void frameLog( const char *fmt, ... )
{
   if ( g_frameDebug )
   {
      va_list argptr;
      fprintf( stderr, "%lld: ", getCurrentTimeMillis());
      va_start( argptr, fmt );
      vfprintf( stderr, fmt, argptr );
      va_end( argptr );
   }
}

static void sbFormat(void *data, struct wl_sb *wl_sb, uint32_t format)
{
   WESTEROS_UNUSED(wl_sb);
   GstWesterosSink *sink= (GstWesterosSink*)data;
   WESTEROS_UNUSED(sink);
   printf("westeros-sink-soc: registry: sbFormat: %X\n", format);
}

static const struct wl_sb_listener sbListener = {
	sbFormat
};

typedef struct bufferInfo
{
   GstWesterosSink *sink;
   int buffIndex;
} bufferInfo;

static void buffer_release( void *data, struct wl_buffer *buffer )
{
   int rc;
   bufferInfo *binfo= (bufferInfo*)data;

   GstWesterosSink *sink= binfo->sink;

   if (binfo->buffIndex >= 0)
   {
      FRAME("out:       wayland release received for buffer %d", binfo->buffIndex);
      LOCK(sink);
      if ( drmUnlockBuffer( sink, binfo->buffIndex ) )
      {
         drmReleaseBuffer( sink, binfo->buffIndex );
      }
      UNLOCK(sink);
   }

   wl_buffer_destroy( buffer );

   free( binfo );
}

static struct wl_buffer_listener wl_buffer_listener=
{
   buffer_release
};

void gst_westeros_sink_soc_class_init(GstWesterosSinkClass *klass)
{
   GObjectClass *gobject_class= (GObjectClass *) klass;
   GstBaseSinkClass *gstbasesink_class= (GstBaseSinkClass *) klass;

   gst_element_class_set_static_metadata( GST_ELEMENT_CLASS(klass),
                                          "Westeros Sink",
                                          "Sink/Video",
                                          "Writes buffers to the westeros wayland compositor",
                                          "Comcast" );

   g_object_class_install_property (gobject_class, PROP_ENABLE_TEXTURE,
     g_param_spec_boolean ("enable-texture",
                           "enable texture signal",
                           "0: disable; 1: enable", FALSE, G_PARAM_READWRITE));

   g_object_class_install_property (gobject_class, PROP_FORCE_ASPECT_RATIO,
     g_param_spec_boolean ("force-aspect-ratio",
                           "force aspect ratio",
                           "When enabled scaling respects source aspect ratio", FALSE, G_PARAM_READWRITE));

   g_signals[SIGNAL_FIRSTFRAME]= g_signal_new( "first-video-frame-callback",
                                               G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
                                               (GSignalFlags) (G_SIGNAL_RUN_LAST),
                                               0,    /* class offset */
                                               NULL, /* accumulator */
                                               NULL, /* accu data */
                                               g_cclosure_marshal_VOID__UINT_POINTER,
                                               G_TYPE_NONE,
                                               2,
                                               G_TYPE_UINT,
                                               G_TYPE_POINTER );

   g_signals[SIGNAL_NEWTEXTURE]= g_signal_new( "new-video-texture-callback",
                                               G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
                                               (GSignalFlags) (G_SIGNAL_RUN_LAST),
                                               0,    /* class offset */
                                               NULL, /* accumulator */
                                               NULL, /* accu data */
                                               NULL,
                                               G_TYPE_NONE,
                                               15,
                                               G_TYPE_UINT, /* format: fourcc */
                                               G_TYPE_UINT, /* pixel width */
                                               G_TYPE_UINT, /* pixel height */
                                               G_TYPE_INT,  /* plane 0 fd */
                                               G_TYPE_UINT, /* plane 0 byte length */
                                               G_TYPE_UINT, /* plane 0 stride */
                                               G_TYPE_POINTER, /* plane 0 data */
                                               G_TYPE_INT,  /* plane 1 fd */
                                               G_TYPE_UINT, /* plane 1 byte length */
                                               G_TYPE_UINT, /* plane 1 stride */
                                               G_TYPE_POINTER, /* plane 1 data */
                                               G_TYPE_INT,  /* plane 2 fd */
                                               G_TYPE_UINT, /* plane 2 byte length */
                                               G_TYPE_UINT, /* plane 2 stride */
                                               G_TYPE_POINTER /* plane 2 data */
                                             );

   #ifdef USE_GST_VIDEO
   g_signals[SIGNAL_TIMECODE]= g_signal_new( "timecode-callback",
                                              G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
                                              (GSignalFlags) (G_SIGNAL_RUN_LAST),
                                              0,    /* class offset */
                                              NULL, /* accumulator */
                                              NULL, /* accu data */
                                              NULL,
                                              G_TYPE_NONE,
                                              3,
                                              G_TYPE_UINT, /* hours */
                                              G_TYPE_UINT, /* minutes */
                                              G_TYPE_UINT  /* seconds */
                                             );
   #endif
}

gboolean gst_westeros_sink_soc_init( GstWesterosSink *sink )
{
   gboolean result= FALSE;
   const char *env;
   int rc;

   #ifdef GLIB_VERSION_2_32
   g_mutex_init( &sink->soc.mutex );
   #else
   sink->soc.mutex= g_mutex_new();
   #endif

   sink->soc.sb= 0;
   sink->soc.frameRate= 0.0;
   sink->soc.frameWidth= -1;
   sink->soc.frameHeight= -1;
   sink->soc.frameFormatStream= 0;
   sink->soc.frameInCount= 0;
   sink->soc.frameOutCount= 0;
   sink->soc.frameDisplayCount= 0;
   sink->soc.numDropped= 0;
   sink->soc.currentInputPTS= 0;

   sink->soc.updateSession= FALSE;
   sink->soc.syncType= -1;
   sink->soc.sessionId= 0;
   sink->soc.videoPlaying= FALSE;
   sink->soc.videoPaused= FALSE;
   sink->soc.quitEOSDetectionThread= FALSE;
   sink->soc.quitDispatchThread= FALSE;
   sink->soc.eosDetectionThread= NULL;
   sink->soc.dispatchThread= NULL;
   sink->soc.emitFirstFrameSignal= FALSE;
   sink->soc.nextFrameFd= -1;
   sink->soc.prevFrame1Fd= -1;
   sink->soc.prevFrame2Fd= -1;
   sink->soc.resubFd= -1;
   sink->soc.captureEnabled= FALSE;
   sink->soc.useCaptureOnly= FALSE;
   sink->soc.hideVideoFramesDelay= 2;
   sink->soc.hideGfxFramesDelay= 1;
   sink->soc.framesBeforeHideVideo= 0;
   sink->soc.framesBeforeHideGfx= 0;
   sink->soc.prevFrameTimeGfx= 0;
   sink->soc.prevFramePTSGfx= 0;
   sink->soc.videoX= sink->windowX;
   sink->soc.videoY= sink->windowY;
   sink->soc.videoWidth= sink->windowWidth;
   sink->soc.videoHeight= sink->windowHeight;
   sink->soc.forceAspectRatio= FALSE;
   sink->soc.drmFd= -1;
   sink->soc.nextDrmBuffer= 0;
   sink->soc.firstFrameThread= NULL;
   {
      int i;
      for( i= 0; i < WST_NUM_DRM_BUFFERS; ++i )
      {
         sink->soc.drmBuffer[i].buffIndex= i;
         sink->soc.drmBuffer[i].locked= false;
         sink->soc.drmBuffer[i].lockCount= 0;
         sink->soc.drmBuffer[i].width= -1;
         sink->soc.drmBuffer[i].height= -1;
         sink->soc.drmBuffer[i].fd0= -1;
         sink->soc.drmBuffer[i].fd1= -1;
         sink->soc.drmBuffer[i].handle0= 0;
         sink->soc.drmBuffer[i].handle1= 0;
      }
   }
   rc= sem_init( &sink->soc.drmBuffSem, 0, WST_NUM_DRM_BUFFERS );
   if ( !rc )
   {
      sink->soc.haveDrmBuffSem= true;
   }
   else
   {
      sink->soc.haveDrmBuffSem= false;
      GST_ERROR( "sem_init failed for drmBuffSem rc %d", rc);
   }

   sink->useSegmentPosition= TRUE;

   /* Request caps updates */
   sink->passCaps= TRUE;

   gst_base_sink_set_sync(GST_BASE_SINK(sink), TRUE);

   gst_base_sink_set_async_enabled(GST_BASE_SINK(sink), TRUE);

   if ( getenv("WESTEROS_SINK_USE_GFX") )
   {
      sink->soc.useCaptureOnly= TRUE;
      sink->soc.captureEnabled= TRUE;
      printf("westeros-sink: capture only\n");
   }

   env= getenv( "WESTEROS_SINK_DEBUG_FRAME" );
   if ( env )
   {
      int level= atoi( env );
      g_frameDebug= (level > 0 ? true : false);
   }

   result= TRUE;

   return result;
}

void gst_westeros_sink_soc_term( GstWesterosSink *sink )
{
   if ( sink->soc.haveDrmBuffSem )
   {
      sink->soc.haveDrmBuffSem= false;
      sem_destroy( &sink->soc.drmBuffSem );
   }
   sem_destroy( &sink->soc.drmBuffSem );
   #ifdef GLIB_VERSION_2_32
   g_mutex_clear( &sink->soc.mutex );
   #else
   g_mutex_free( sink->soc.mutex );
   #endif
}

void gst_westeros_sink_soc_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
   GstWesterosSink *sink = GST_WESTEROS_SINK(object);

   WESTEROS_UNUSED(pspec);

   switch (prop_id)
   {
      case PROP_FORCE_ASPECT_RATIO:
         {
            sink->soc.forceAspectRatio= g_value_get_boolean(value);
            break;
         }
      case PROP_ENABLE_TEXTURE:
         {
            sink->soc.enableTextureSignal= g_value_get_boolean(value);
         }
         break;
      default:
         G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
         break;
   }
}

void gst_westeros_sink_soc_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
   GstWesterosSink *sink = GST_WESTEROS_SINK(object);

   WESTEROS_UNUSED(pspec);

   switch (prop_id)
   {
      case PROP_FORCE_ASPECT_RATIO:
         g_value_set_boolean(value, sink->soc.forceAspectRatio);
         break;
      case PROP_ENABLE_TEXTURE:
         g_value_set_boolean(value, sink->soc.enableTextureSignal);
         break;
      default:
         G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
         break;
   }
}

void gst_westeros_sink_soc_registryHandleGlobal( GstWesterosSink *sink,
                                 struct wl_registry *registry, uint32_t id,
		                           const char *interface, uint32_t version)
{
   WESTEROS_UNUSED(version);
   int len;

   len= strlen(interface);

   if ((len==5) && (strncmp(interface, "wl_sb", len) == 0))
   {
      sink->soc.sb= (struct wl_sb*)wl_registry_bind(registry, id, &wl_sb_interface, version);
      printf("westeros-sink-soc: registry: sb %p\n", (void*)sink->soc.sb);
      wl_proxy_set_queue((struct wl_proxy*)sink->soc.sb, sink->queue);
		wl_sb_add_listener(sink->soc.sb, &sbListener, sink);
		printf("westeros-sink-soc: registry: done add sb listener\n");
   }

   if ( sink->soc.useCaptureOnly )
   {
      /* Don't use vpc when capture only */
      if ( sink->vpc )
      {
         wl_vpc_destroy( sink->vpc );
         sink->vpc= 0;
      }
   }
}

void gst_westeros_sink_soc_registryHandleGlobalRemove( GstWesterosSink *sink,
                                 struct wl_registry *registry,
			                        uint32_t name)
{
   WESTEROS_UNUSED(sink);
   WESTEROS_UNUSED(registry);
   WESTEROS_UNUSED(name);
}

gboolean gst_westeros_sink_soc_null_to_ready( GstWesterosSink *sink, gboolean *passToDefault )
{
   gboolean result= FALSE;

   WESTEROS_UNUSED(passToDefault);
   if ( drmInit( sink ) )
   {
      result= TRUE;
   }

   return result;
}

gboolean gst_westeros_sink_soc_ready_to_paused( GstWesterosSink *sink, gboolean *passToDefault )
{
   WESTEROS_UNUSED(passToDefault);

   if ( !sink->soc.useCaptureOnly )
   {
      sink->soc.conn= wstCreateVideoClientConnection( sink, DEFAULT_VIDEO_SERVER );
      if ( !sink->soc.conn )
      {
         GST_ERROR("unable to connect to video server (%s)", DEFAULT_VIDEO_SERVER );
         sink->soc.useCaptureOnly= TRUE;
         sink->soc.captureEnabled= TRUE;
         printf("westeros-sink: no video server - capture only\n");
         if ( sink->vpc )
         {
            wl_vpc_destroy( sink->vpc );
            sink->vpc= 0;
         }
      }
   }

   LOCK(sink);
   sink->startAfterCaps= TRUE;
   sink->soc.videoPlaying= TRUE;
   sink->soc.videoPaused= FALSE;
   UNLOCK(sink);

   return TRUE;
}

gboolean gst_westeros_sink_soc_paused_to_playing( GstWesterosSink *sink, gboolean *passToDefault )
{
   WESTEROS_UNUSED(passToDefault);

   LOCK( sink );
   sink->soc.videoPlaying= TRUE;
   sink->soc.videoPaused= FALSE;
   sink->soc.updateSession= TRUE;
   UNLOCK( sink );

   return TRUE;
}

gboolean gst_westeros_sink_soc_playing_to_paused( GstWesterosSink *sink, gboolean *passToDefault )
{
   LOCK( sink );
   sink->soc.videoPlaying= FALSE;
   sink->soc.videoPaused= TRUE;
   UNLOCK( sink );

   if (gst_base_sink_is_async_enabled(GST_BASE_SINK(sink)))
   {
      *passToDefault= true;
   }
   else
   {
      *passToDefault = false;
   }

   return TRUE;
}

gboolean gst_westeros_sink_soc_paused_to_ready( GstWesterosSink *sink, gboolean *passToDefault )
{
   wstSinkSocStopVideo( sink );
   LOCK( sink );
   sink->videoStarted= FALSE;
   UNLOCK( sink );

   if (gst_base_sink_is_async_enabled(GST_BASE_SINK(sink)))
   {
      *passToDefault= true;
   }
   else
   {
      *passToDefault= false;
   }

   return TRUE;
}

gboolean gst_westeros_sink_soc_ready_to_null( GstWesterosSink *sink, gboolean *passToDefault )
{
   WESTEROS_UNUSED(sink);

   wstSinkSocStopVideo( sink );

   drmTerm( sink );

   *passToDefault= false;

   return TRUE;
}

gboolean gst_westeros_sink_soc_accept_caps( GstWesterosSink *sink, GstCaps *caps )
{
   bool result= FALSE;
   GstStructure *structure;
   const gchar *mime;
   int len;

   gchar *str= gst_caps_to_string(caps);
   g_print("westeros-sink: caps: (%s)\n", str);
   g_free( str );

   structure= gst_caps_get_structure(caps, 0);
   if( structure )
   {
      mime= gst_structure_get_name(structure);
      if ( mime )
      {
         len= strlen(mime);
         if ( (len == 11) && !strncmp("video/x-raw", mime, len) )
         {
            result= TRUE;
         }
         else
         {
            GST_ERROR("gst_westeros_sink_soc_accept_caps: not accepting caps (%s)", mime );
         }
      }

      if ( result == TRUE )
      {
         gint num, denom, width, height;
         const gchar *format= 0;
         if ( gst_structure_get_fraction( structure, "framerate", &num, &denom ) )
         {
            if ( denom == 0 ) denom= 1;
            sink->soc.frameRate= (double)num/(double)denom;
            if ( sink->soc.frameRate <= 0.0 )
            {
               g_print("westeros-sink: caps have framerate of 0 - assume 60\n");
               sink->soc.frameRate= 60.0;
            }
         }
         if ( gst_structure_get_int( structure, "width", &width ) )
         {
            sink->soc.frameWidth= width;
         }
         if ( gst_structure_get_int( structure, "height", &height ) )
         {
            sink->soc.frameHeight= height;
         }
         format= gst_structure_get_string( structure, "format" );
         if ( format )
         {
            int len= strlen(format);
            if ( (len == 4) && !strncmp( format, "NV12", len) )
            {
               sink->soc.frameFormatStream= DRM_FORMAT_NV12;
            }
            else if ( (len == 4) && !strncmp( format, "I420", len) )
            {
               sink->soc.frameFormatStream= DRM_FORMAT_YUV420;
            }
            else
            {
               g_print("format (%s) not supported\n", format);
               result= FALSE;
            }
         }
      }
   }

   return result;
}

void gst_westeros_sink_soc_set_startPTS( GstWesterosSink *sink, gint64 pts )
{
   WESTEROS_UNUSED(sink);
   WESTEROS_UNUSED(pts);
}

void gst_westeros_sink_soc_render( GstWesterosSink *sink, GstBuffer *buffer )
{
   while ( sink->soc.videoPaused )
   {
      bool active= true;
      usleep( 1000 );
      wstProcessMessagesVideoClientConnection( sink->soc.conn );
      LOCK(sink);
      if ( sink->flushStarted || !sink->videoStarted )
      {
         active= false;
      }
      UNLOCK(sink);
      if ( !active )
      {
         return;
      }
   }
   if ( !sink->flushStarted )
   {
      gint64 nanoTime;
      int rc, buffIndex;
      int inSize, offset, avail, copylen;
      unsigned char *inData;
      WstDrmBuffer *drmBuff= 0;
      #ifdef USE_GST1
      GstMapInfo map;
      gst_buffer_map(buffer, &map, (GstMapFlags)GST_MAP_READ);
      inSize= map.size;
      inData= map.data;
      #else
      inSize= (int)GST_BUFFER_SIZE(buffer);
      inData= GST_BUFFER_DATA(buffer);
      #endif
      #ifdef USE_GST_ALLOCATORS
      GstMemory *mem;

      ++sink->soc.frameInCount;

      mem= gst_buffer_peek_memory( buffer, 0 );
      if ( gst_is_dmabuf_memory(mem) )
      {
         /* TBD: support gstreamer buffers backed by dma-buf */
         g_print("Warning: using dma-buf for input: not supported yet");
      }
      #endif

      GST_LOG("gst_westeros_sink_soc_render: buffer %p, len %d timestamp: %lld", buffer, inSize, GST_BUFFER_PTS(buffer) );

      if ( GST_BUFFER_PTS_IS_VALID(buffer) )
      {
         guint64 prevPTS;

         nanoTime= GST_BUFFER_PTS(buffer);
         {
            guint64 gstNow= getGstClockTime(sink);
            if ( gstNow <= nanoTime )
               FRAME("in: frame PTS %lld gst clock %lld: lead time %lld us", nanoTime, gstNow, (nanoTime-gstNow)/1000LL);
            else
               FRAME("in: frame PTS %lld gst clock %lld: lead time %lld us", nanoTime, gstNow, (gstNow-nanoTime)/1000LL);
         }
         LOCK(sink)
         if ( nanoTime >= sink->segment.start )
         {
            if ( sink->prevPositionSegmentStart == 0xFFFFFFFFFFFFFFFFLL )
            {
               sink->soc.currentInputPTS= 0;
            }
            prevPTS= sink->soc.currentInputPTS;
            sink->soc.currentInputPTS= ((nanoTime / GST_SECOND) * 90000)+(((nanoTime % GST_SECOND) * 90000) / GST_SECOND);
            if (sink->prevPositionSegmentStart != sink->positionSegmentStart)
            {
               sink->firstPTS= sink->soc.currentInputPTS;
               sink->prevPositionSegmentStart = sink->positionSegmentStart;
               GST_DEBUG("SegmentStart changed! Updating first PTS to %lld ", sink->firstPTS);
            }
            if ( sink->soc.currentInputPTS != 0 || sink->soc.frameInCount == 0 )
            {
               if ( (sink->soc.currentInputPTS < sink->firstPTS) && (sink->soc.currentInputPTS > 90000) )
               {
                  /* If we have hit a discontinuity that doesn't look like rollover, then
                     treat this as the case of looping a short clip.  Adjust our firstPTS
                     to keep our running time correct. */
                  sink->firstPTS= sink->firstPTS-(prevPTS-sink->soc.currentInputPTS);
               }
            }
         }
         UNLOCK(sink);
      }

      if ( sink->display )
      {
         if ( sink->soc.dispatchThread == NULL )
         {
            sink->soc.quitDispatchThread= FALSE;
            GST_DEBUG_OBJECT(sink, "starting westeros_sink_dispatch thread");
            sink->soc.dispatchThread= g_thread_new("westeros_sink_dispatch", wstDispatchThread, sink);
         }
      }
      if ( sink->soc.eosDetectionThread == NULL )
      {
         sink->soc.videoPlaying= TRUE;;
         sink->eosEventSeen= TRUE;
         sink->soc.quitEOSDetectionThread= FALSE;
         GST_DEBUG_OBJECT(sink, "starting westeros_sink_eos thread");
         sink->soc.eosDetectionThread= g_thread_new("westeros_sink_eos", wstEOSDetectionThread, sink);
      }

      if ( inSize )
      {
         drmBuff= drmGetBuffer( sink, sink->soc.frameWidth, sink->soc.frameHeight );
         if ( drmBuff )
         {
            unsigned char *data;
            unsigned char *Y, *U, *V;
            int Ystride, Ustride, Vstride;

            if ( !sink->videoStarted )
            {
               sink->videoStarted= TRUE;
               wstSetSessionInfo( sink );
            }

            buffIndex= drmBuff->buffIndex;

            switch( sink->soc.frameFormatStream )
            {
               case DRM_FORMAT_NV12:
                  Y= inData;
                  Ystride= ((sink->soc.frameWidth + 3) & ~3);
                  U= Y + Ystride*sink->soc.frameHeight;
                  Ustride= Ystride;
                  V= 0;
                  Vstride= 0;
                  break;
               case DRM_FORMAT_YUV420:
                  Y= inData;
                  Ystride= ((sink->soc.frameWidth + 3) & ~3);
                  U= Y + Ystride*sink->soc.frameHeight;
                  Ustride= Ystride/2;
                  V= U + Ustride*sink->soc.frameHeight/2;
                  Vstride= Ystride/2;
                  break;
               default:
                  Y= U= V= 0;
                  break;
            }

            if ( Y )
            {
               data= (unsigned char*)mmap( NULL, drmBuff->size0, PROT_READ | PROT_WRITE, MAP_SHARED, sink->soc.drmFd, drmBuff->offset0 );
               if ( data )
               {
                  int row;
                  unsigned char *destRow= data;
                  unsigned char *srcYRow= Y;
                  for( row= 0; row < sink->soc.frameHeight; ++row )
                  {
                     memcpy( destRow, srcYRow, Ystride );
                     destRow += drmBuff->pitch0;
                     srcYRow += Ystride;
                  }
                  munmap( data, drmBuff->size0 );
               }
               if ( U && !V )
               {
                  data= (unsigned char*)mmap( NULL, drmBuff->size1, PROT_READ | PROT_WRITE, MAP_SHARED, sink->soc.drmFd, drmBuff->offset1 );
                  if ( data )
                  {
                     int row;
                     unsigned char *destRow= data;
                     unsigned char *srcURow= U;
                     for( row= 0; row < sink->soc.frameHeight; ++row )
                     {
                        memcpy( destRow, srcURow, Ustride );
                        destRow += drmBuff->pitch1;
                        srcURow += Ustride;
                     }
                     munmap( data, drmBuff->size1 );
                  }
               }
               if ( U && V )
               {
                  data= (unsigned char*)mmap( NULL, drmBuff->size1, PROT_READ | PROT_WRITE, MAP_SHARED, sink->soc.drmFd, drmBuff->offset1 );
                  if ( data )
                  {
                     int row, col;
                     unsigned char *dest, *destRow= data;
                     unsigned char *srcU, *srcURow= U;
                     unsigned char *srcV, *srcVRow= V;
                     for( row= 0; row < sink->soc.frameHeight; row += 2 )
                     {
                        dest= destRow;
                        srcU= srcURow;
                        srcV= srcVRow;
                        for( col= 0; col < sink->soc.frameWidth; col += 2 )
                        {
                           *dest++= *srcU++;
                           *dest++= *srcV++;
                        }
                        destRow += drmBuff->pitch1;
                        srcURow += Ustride;
                        srcVRow += Vstride;
                     }
                     munmap( data, drmBuff->size1 );
                  }
               }

               if ( sink->soc.frameOutCount == 0 )
               {
                  sink->soc.firstFrameThread= g_thread_new("westeros_first_frame", wstFirstFrameThread, sink);
               }

               drmBuff->frameTime= ((GST_BUFFER_PTS(buffer) + 500LL) / 1000LL);

               if ( !sink->soc.conn )
               {
                  /* If we are not connected to a video server, set position here */
                  gint64 frameTime= GST_BUFFER_PTS(buffer);
                  gint64 firstNano= ((sink->firstPTS/90LL)*GST_MSECOND)+((sink->firstPTS%90LL)*GST_MSECOND/90LL);
                  sink->position= sink->positionSegmentStart + frameTime - firstNano;
                  sink->currentPTS= frameTime / (GST_SECOND/90000LL);
                  if ( sink->timeCodePresent && sink->enableTimeCodeSignal )
                  {
                     sink->timeCodePresent( sink, sink->position, g_signals[SIGNAL_TIMECODE] );
                  }
               }

               if ( sink->soc.enableTextureSignal )
               {
                  int fd0, l0, s0, fd1, l1, fd2, s1, l2, s2;
                  void *p0, *p1, *p2;

                  fd0= drmBuff->fd0;
                  fd1= drmBuff->fd1;
                  fd2= -1;
                  s0= drmBuff->pitch0;
                  s1= drmBuff->pitch1;
                  s2= 0;
                  l0= drmBuff->size0;
                  l1= drmBuff->size1;
                  l2= 0;
                  p0= 0;
                  p1= 0;
                  p2= 0;

                  g_signal_emit( G_OBJECT(sink),
                                 g_signals[SIGNAL_NEWTEXTURE],
                                 0,
                                 DRM_FORMAT_NV12,
                                 sink->soc.frameWidth,
                                 sink->soc.frameHeight,
                                 fd0, l0, s0, p0,
                                 fd1, l1, s1, p1,
                                 fd2, l2, s2, p2
                               );
               }
               else if ( sink->soc.captureEnabled && sink->soc.sb )
               {
                  bufferInfo *binfo;
                  binfo= (bufferInfo*)malloc( sizeof(bufferInfo) );
                  if ( binfo )
                  {
                     struct wl_buffer *wlbuff;
                     int fd0, fd1, fd2;
                     int stride0, stride1;
                     int offset1= 0;
                     fd0= drmBuff->fd0;
                     fd1= drmBuff->fd1;
                     fd2= fd0;
                     stride0= drmBuff->pitch0;
                     stride1= drmBuff->pitch1;

                     binfo->sink= sink;
                     binfo->buffIndex= buffIndex;

                     wlbuff= wl_sb_create_planar_buffer_fd2( sink->soc.sb,
                                                             fd0,
                                                             fd1,
                                                             fd2,
                                                             drmBuff->width,
                                                             drmBuff->height,
                                                             WL_SB_FORMAT_NV12,
                                                             0, /* offset0 */
                                                             offset1, /* offset1 */
                                                             0, /* offset2 */
                                                             stride0, /* stride0 */
                                                             stride1, /* stride1 */
                                                             0  /* stride2 */
                                                           );
                     if ( wlbuff )
                     {
                        wl_buffer_add_listener( wlbuff, &wl_buffer_listener, binfo );
                        wl_surface_attach( sink->surface, wlbuff, sink->windowX, sink->windowY );
                        wl_surface_damage( sink->surface, 0, 0, sink->windowWidth, sink->windowHeight );
                        wl_surface_commit( sink->surface );
                        wl_display_flush(sink->display);

                        drmLockBuffer( sink, buffIndex );

                        /* Advance any frames sent to video server towards requeueing to decoder */
                        sink->soc.resubFd= sink->soc.prevFrame2Fd;
                        sink->soc.prevFrame2Fd=sink->soc.prevFrame1Fd;
                        sink->soc.prevFrame1Fd= sink->soc.nextFrameFd;
                        sink->soc.nextFrameFd= -1;

                        if ( sink->soc.framesBeforeHideVideo )
                        {
                           if ( --sink->soc.framesBeforeHideVideo == 0 )
                           {
                              wstSendHideVideoClientConnection( sink->soc.conn, true );
                           }
                        }
                     }
                     else
                     {
                        free( binfo );
                     }
                  }
               }
               if ( sink->soc.conn )
               {
                  sink->soc.resubFd= sink->soc.prevFrame2Fd;
                  sink->soc.prevFrame2Fd= sink->soc.prevFrame1Fd;
                  sink->soc.prevFrame1Fd= sink->soc.nextFrameFd;
                  sink->soc.nextFrameFd= sink->soc.drmBuffer[buffIndex].fd0;

                  if ( wstSendFrameVideoClientConnection( sink->soc.conn, buffIndex ) )
                  {
                     buffIndex= -1;
                  }

                  if ( sink->soc.framesBeforeHideGfx )
                  {
                     if ( --sink->soc.framesBeforeHideGfx == 0 )
                     {
                        wl_surface_attach( sink->surface, 0, sink->windowX, sink->windowY );
                        wl_surface_damage( sink->surface, 0, 0, sink->windowWidth, sink->windowHeight );
                        wl_surface_commit( sink->surface );
                        wl_display_flush(sink->display);
                        wl_display_dispatch_queue_pending(sink->display, sink->queue);
                        wstSendHideVideoClientConnection( sink->soc.conn, false );
                     }
                  }
               }
            }
         }
         LOCK(sink);
         ++sink->soc.frameOutCount;
         UNLOCK(sink);
      }

      #ifdef USE_GST1
      gst_buffer_unmap( buffer, &map);
      #endif
   }
}

void gst_westeros_sink_soc_flush( GstWesterosSink *sink )
{
   GST_DEBUG("gst_westeros_sink_soc_flush");
   if ( sink->videoStarted )
   {
      LOCK(sink);
      sink->videoStarted= FALSE;
      UNLOCK(sink);
      sink->startAfterCaps= TRUE;
      sink->soc.prevFrameTimeGfx= 0;
      sink->soc.prevFramePTSGfx= 0;
      sink->soc.prevFrame1Fd= -1;
      sink->soc.prevFrame2Fd= -1;
      sink->soc.nextFrameFd= -1;
   }
   LOCK(sink);
   sink->soc.frameInCount= 0;
   sink->soc.frameOutCount= 0;
   sink->soc.frameDisplayCount= 0;
   sink->soc.numDropped= 0;
   sink->soc.frameDisplayCount= FALSE;
   UNLOCK(sink);
}

gboolean gst_westeros_sink_soc_start_video( GstWesterosSink *sink )
{
   WESTEROS_UNUSED(sink);
}

void gst_westeros_sink_soc_eos_event( GstWesterosSink *sink )
{
   WESTEROS_UNUSED(sink);
}

void gst_westeros_sink_soc_set_video_path( GstWesterosSink *sink, bool useGfxPath )
{
   if ( useGfxPath && !sink->soc.captureEnabled )
   {
      sink->soc.captureEnabled= TRUE;

      sink->soc.framesBeforeHideVideo= sink->soc.hideVideoFramesDelay;
   }
   else if ( !useGfxPath && sink->soc.captureEnabled )
   {
      sink->soc.captureEnabled= FALSE;
      sink->soc.prevFrame1Fd= -1;
      sink->soc.prevFrame2Fd= -1;
      sink->soc.nextFrameFd= -1;
      sink->soc.framesBeforeHideGfx= sink->soc.hideGfxFramesDelay;
   }
   if ( sink->soc.forceAspectRatio && sink->vpcSurface )
   {
      /* Use nominal display size provided to us by
       * the compositor to calculate the video bounds
       * we should use when we transition to graphics path.
       * Save and restore current HW video rectangle. */
      int vx, vy, vw, vh;
      int tx, ty, tw, th;
      tx= sink->soc.videoX;
      ty= sink->soc.videoY;
      tw= sink->soc.videoWidth;
      th= sink->soc.videoHeight;
      sink->soc.videoX= sink->windowX;
      sink->soc.videoY= sink->windowY;
      sink->soc.videoWidth= sink->windowWidth;
      sink->soc.videoHeight= sink->windowHeight;

      wstGetVideoBounds( sink, &vx, &vy, &vw, &vh );
      wl_vpc_surface_set_geometry( sink->vpcSurface, vx, vy, vw, vh );

      sink->soc.videoX= tx;
      sink->soc.videoY= ty;
      sink->soc.videoWidth= tw;
      sink->soc.videoHeight= th;
   }
}

void gst_westeros_sink_soc_update_video_position( GstWesterosSink *sink )
{
   if ( sink->windowSizeOverride )
   {
      sink->soc.videoX= ((sink->windowX*sink->scaleXNum)/sink->scaleXDenom) + sink->transX;
      sink->soc.videoY= ((sink->windowY*sink->scaleYNum)/sink->scaleYDenom) + sink->transY;
      sink->soc.videoWidth= (sink->windowWidth*sink->scaleXNum)/sink->scaleXDenom;
      sink->soc.videoHeight= (sink->windowHeight*sink->scaleYNum)/sink->scaleYDenom;
   }
   else
   {
      sink->soc.videoX= sink->transX;
      sink->soc.videoY= sink->transY;
      sink->soc.videoWidth= (sink->outputWidth*sink->scaleXNum)/sink->scaleXDenom;
      sink->soc.videoHeight= (sink->outputHeight*sink->scaleYNum)/sink->scaleYDenom;
   }

   if ( !sink->soc.captureEnabled )
   {
      /* Send a buffer to compositor to update hole punch geometry */
      if ( sink->soc.sb )
      {
         struct wl_buffer *buff;

         buff= wl_sb_create_buffer( sink->soc.sb,
                                    0,
                                    sink->windowWidth,
                                    sink->windowHeight,
                                    sink->windowWidth*4,
                                    WL_SB_FORMAT_ARGB8888 );
         wl_surface_attach( sink->surface, buff, sink->windowX, sink->windowY );
         wl_surface_damage( sink->surface, 0, 0, sink->windowWidth, sink->windowHeight );
         wl_surface_commit( sink->surface );
      }
   }
}

gboolean gst_westeros_sink_soc_query( GstWesterosSink *sink, GstQuery *query )
{
}

static void wstSinkSocStopVideo( GstWesterosSink *sink )
{
   LOCK(sink);
   if ( sink->soc.conn )
   {
      wstDestroyVideoClientConnection( sink->soc.conn );
      sink->soc.conn= 0;
   }
   if ( sink->soc.eosDetectionThread || sink->soc.dispatchThread )
   {
      sink->soc.quitEOSDetectionThread= TRUE;
      sink->soc.quitDispatchThread= TRUE;
      if ( sink->display )
      {
         int fd= wl_display_get_fd( sink->display );
         if ( fd >= 0 )
         {
            shutdown( fd, SHUT_RDWR );
         }
      }
   }
   drmUnlockAllBuffers( sink );
   UNLOCK(sink);

   sink->soc.prevFrame1Fd= -1;
   sink->soc.prevFrame2Fd= -1;
   sink->soc.nextFrameFd= -1;
   sink->soc.frameWidth= -1;
   sink->soc.frameHeight= -1;
   sink->soc.syncType= -1;
   sink->soc.emitFirstFrameSignal= FALSE;

   LOCK(sink);
   sink->videoStarted= FALSE;
   UNLOCK(sink);

   if ( sink->soc.eosDetectionThread )
   {
      sink->soc.quitEOSDetectionThread= TRUE;
      g_thread_join( sink->soc.eosDetectionThread );
      sink->soc.eosDetectionThread= NULL;
   }

   if ( sink->soc.dispatchThread )
   {
      sink->soc.quitDispatchThread= TRUE;
      g_thread_join( sink->soc.dispatchThread );
      sink->soc.dispatchThread= NULL;
   }

   if ( sink->soc.sb )
   {
      wl_sb_destroy( sink->soc.sb );
      sink->soc.sb= 0;
   }
}

static void wstGetVideoBounds( GstWesterosSink *sink, int *x, int *y, int *w, int *h )
{
   int vx, vy, vw, vh;
   double arf, ard;
   vx= sink->soc.videoX;
   vy= sink->soc.videoY;
   vw= sink->soc.videoWidth;
   vh= sink->soc.videoHeight;
   ard= (double)sink->soc.videoWidth/(double)sink->soc.videoHeight;
   arf= (double)sink->soc.frameWidth/(double)sink->soc.frameHeight;
   if ( arf >= ard )
   {
      vh= (sink->soc.frameHeight * sink->soc.videoWidth) / sink->soc.frameWidth;
      vy= vy+(sink->soc.videoHeight-vh)/2;
   }
   else
   {
      vw= (sink->soc.frameWidth * sink->soc.videoHeight) / sink->soc.frameHeight;
      vx= vx+(sink->soc.videoWidth-vw)/2;
   }
   *x= vx;
   *y= vy;
   *w= vw;
   *h= vh;
}

static WstVideoClientConnection *wstCreateVideoClientConnection( GstWesterosSink *sink, const char *name )
{
   WstVideoClientConnection *conn= 0;
   int rc;
   bool error= true;
   const char *workingDir;
   int pathNameLen, addressSize;

   conn= (WstVideoClientConnection*)calloc( 1, sizeof(WstVideoClientConnection));
   if ( conn )
   {
      conn->socketFd= -1;
      conn->name= name;
      conn->sink= sink;

      workingDir= getenv("XDG_RUNTIME_DIR");
      if ( !workingDir )
      {
         GST_ERROR("wstCreateVideoClientConnection: XDG_RUNTIME_DIR is not set");
         goto exit;
      }

      pathNameLen= strlen(workingDir)+strlen("/")+strlen(conn->name)+1;
      if ( pathNameLen > (int)sizeof(conn->addr.sun_path) )
      {
         GST_ERROR("wstCreateVideoClientConnection: name for server unix domain socket is too long: %d versus max %d",
                pathNameLen, (int)sizeof(conn->addr.sun_path) );
         goto exit;
      }

      conn->addr.sun_family= AF_LOCAL;
      strcpy( conn->addr.sun_path, workingDir );
      strcat( conn->addr.sun_path, "/" );
      strcat( conn->addr.sun_path, conn->name );

      conn->socketFd= socket( PF_LOCAL, SOCK_STREAM|SOCK_CLOEXEC, 0 );
      if ( conn->socketFd < 0 )
      {
         GST_ERROR("wstCreateVideoClientConnection: unable to open socket: errno %d", errno );
         goto exit;
      }

      addressSize= pathNameLen + offsetof(struct sockaddr_un, sun_path);

      rc= connect(conn->socketFd, (struct sockaddr *)&conn->addr, addressSize );
      if ( rc < 0 )
      {
         GST_ERROR("wstCreateVideoClientConnection: connect failed for socket: errno %d", errno );
         goto exit;
      }

      error= false;
   }

exit:

   if ( error )
   {
      wstDestroyVideoClientConnection( conn );
      conn= 0;
   }

   return conn;
}

static void wstDestroyVideoClientConnection( WstVideoClientConnection *conn )
{
   if ( conn )
   {
      conn->addr.sun_path[0]= '\0';

      if ( conn->socketFd >= 0 )
      {
         close( conn->socketFd );
         conn->socketFd= -1;
      }

      free( conn );
   }
}

static unsigned int getU32( unsigned char *p )
{
   unsigned n;

   n= (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|(p[3]);

   return n;
}

static int putU32( unsigned char *p, unsigned n )
{
   p[0]= (n>>24);
   p[1]= (n>>16);
   p[2]= (n>>8);
   p[3]= (n&0xFF);

   return 4;
}

static gint64 getS64( unsigned char *p )
{
   gint64 n;

   n= ((((gint64)(p[0]))<<56) |
       (((gint64)(p[1]))<<48) |
       (((gint64)(p[2]))<<40) |
       (((gint64)(p[3]))<<32) |
       (((gint64)(p[4]))<<24) |
       (((gint64)(p[5]))<<16) |
       (((gint64)(p[6]))<<8) |
       (p[7]) );

   return n;
}

static int putS64( unsigned char *p,  gint64 n )
{
   p[0]= (((guint64)n)>>56);
   p[1]= (((guint64)n)>>48);
   p[2]= (((guint64)n)>>40);
   p[3]= (((guint64)n)>>32);
   p[4]= (((guint64)n)>>24);
   p[5]= (((guint64)n)>>16);
   p[6]= (((guint64)n)>>8);
   p[7]= (((guint64)n)&0xFF);

   return 8;
}

static void wstSendHideVideoClientConnection( WstVideoClientConnection *conn, bool hide )
{
   if ( conn )
   {
      struct msghdr msg;
      struct iovec iov[1];
      unsigned char mbody[7];
      int len;
      int sentLen;

      msg.msg_name= NULL;
      msg.msg_namelen= 0;
      msg.msg_iov= iov;
      msg.msg_iovlen= 1;
      msg.msg_control= 0;
      msg.msg_controllen= 0;
      msg.msg_flags= 0;

      len= 0;
      mbody[len++]= 'V';
      mbody[len++]= 'S';
      mbody[len++]= 2;
      mbody[len++]= 'H';
      mbody[len++]= (hide ? 1 : 0);

      iov[0].iov_base= (char*)mbody;
      iov[0].iov_len= len;

      do
      {
         sentLen= sendmsg( conn->socketFd, &msg, MSG_NOSIGNAL );
      }
      while ( (sentLen < 0) && (errno == EINTR));

      if ( sentLen == len )
      {
         GST_LOG("sent hide %d to video server", hide);
         FRAME("sent hide %d to video server", hide);
      }
   }
}

static void wstSendSessionInfoVideoClientConnection( WstVideoClientConnection *conn )
{
   if ( conn )
   {
      GstWesterosSink *sink= conn->sink;
      struct msghdr msg;
      struct iovec iov[1];
      unsigned char mbody[9];
      int len;
      int sentLen;

      msg.msg_name= NULL;
      msg.msg_namelen= 0;
      msg.msg_iov= iov;
      msg.msg_iovlen= 1;
      msg.msg_control= 0;
      msg.msg_controllen= 0;
      msg.msg_flags= 0;

      len= 0;
      mbody[len++]= 'V';
      mbody[len++]= 'S';
      mbody[len++]= 6;
      mbody[len++]= 'I';
      mbody[len++]= sink->soc.syncType;
      len += putU32( &mbody[len], conn->sink->soc.sessionId );

      iov[0].iov_base= (char*)mbody;
      iov[0].iov_len= len;

      do
      {
         sentLen= sendmsg( conn->socketFd, &msg, MSG_NOSIGNAL );
      }
      while ( (sentLen < 0) && (errno == EINTR));

      if ( sentLen == len )
      {
         GST_DEBUG("sent session info: type %d sessionId %d to video server", sink->soc.syncType, sink->soc.sessionId);
         g_print("sent session info: type %d sessionId %d to video server\n", sink->soc.syncType, sink->soc.sessionId);
      }
   }
}

#ifdef USE_AMLOGIC_MESON
static GstElement* wstFindAudioSink( GstWesterosSink *sink )
{
   GstElement *audioSink= 0;
   GstElement *pipeline= 0;
   GstElement *element, *elementPrev= 0;
   GstIterator *iterator;

   element= GST_ELEMENT_CAST(sink);
   do
   {
      if ( elementPrev )
      {
         gst_object_unref( elementPrev );
      }
      element= GST_ELEMENT_CAST(gst_element_get_parent( element ));
      if ( element )
      {
         elementPrev= pipeline;
         pipeline= element;
      }
   }
   while( element != 0 );

   if ( pipeline )
   {
      GstIterator *iterElement= gst_bin_iterate_recurse( GST_BIN(pipeline) );
      if ( iterElement )
      {
         GValue itemElement= G_VALUE_INIT;
         while( gst_iterator_next( iterElement, &itemElement ) == GST_ITERATOR_OK )
         {
            element= (GstElement*)g_value_get_object( &itemElement );
            if ( element && !GST_IS_BIN(element) )
            {
               int numSrcPads= 0;

               GstIterator *iterPad= gst_element_iterate_src_pads( element );
               if ( iterPad )
               {
                  GValue itemPad= G_VALUE_INIT;
                  while( gst_iterator_next( iterPad, &itemPad ) == GST_ITERATOR_OK )
                  {
                     GstPad *pad= (GstPad*)g_value_get_object( &itemPad );
                     if ( pad )
                     {
                        ++numSrcPads;
                     }
                     g_value_reset( &itemPad );
                  }
                  gst_iterator_free(iterPad);
               }

               if ( numSrcPads == 0 )
               {
                  GstElementClass *ec= GST_ELEMENT_GET_CLASS(element);
                  if ( ec )
                  {
                     const gchar *meta= gst_element_class_get_metadata( ec, GST_ELEMENT_METADATA_KLASS);
                     if ( meta && strstr(meta, "Sink") && strstr(meta, "Audio") )
                     {
                        audioSink= (GstElement*)gst_object_ref( element );
                        gchar *name= gst_element_get_name( element );
                        if ( name )
                        {
                           GST_DEBUG( "detected audio sink: name (%s)", name);
                           g_free( name );
                        }
                        g_value_reset( &itemElement );
                        break;
                     }
                  }
               }
            }
            g_value_reset( &itemElement );
         }
         gst_iterator_free(iterElement);
      }

      gst_object_unref(pipeline);
   }
   return audioSink;
}
#endif

static void wstSetSessionInfo( GstWesterosSink *sink )
{
   #ifdef USE_AMLOGIC_MESON
   if ( sink->soc.conn )
   {
      GstElement *audioSink;
      GstElement *element= GST_ELEMENT(sink);
      GstClock *clock= GST_ELEMENT_CLOCK(element);
      int syncTypePrev= sink->soc.syncType;
      int sessionIdPrev= sink->soc.sessionId;
      sink->soc.syncType= 0;
      sink->soc.sessionId= 0;
      audioSink= wstFindAudioSink( sink );
      if ( audioSink )
      {
         sink->soc.syncType= 1;
         gst_object_unref( audioSink );
      }
      if ( clock )
      {
         const char *socClockName;
         gchar *clockName;
         clockName= gst_object_get_name(GST_OBJECT_CAST(clock));
         if ( clockName )
         {
            int sclen;
            int len= strlen(clockName);
            socClockName= getenv("WESTEROS_SINK_CLOCK");
            if ( !socClockName )
            {
               socClockName= "GstAmlSinkClock";
            }
            sclen= strlen(socClockName);
            if ( (len == sclen) && !strncmp(clockName, socClockName, len) )
            {
               sink->soc.syncType= 1;
               /* TBD: set sessionid */
            }
            g_free( clockName );
         }
      }
      if ( (syncTypePrev != sink->soc.syncType) || (sessionIdPrev != sink->soc.sessionId) )
      {
         wstSendSessionInfoVideoClientConnection( sink->soc.conn );
      }
   }
   #endif
}

static void wstSendFlushVideoClientConnection( WstVideoClientConnection *conn )
{
   if ( conn )
   {
      struct msghdr msg;
      struct iovec iov[1];
      unsigned char mbody[4];
      int len;
      int sentLen;

      msg.msg_name= NULL;
      msg.msg_namelen= 0;
      msg.msg_iov= iov;
      msg.msg_iovlen= 1;
      msg.msg_control= 0;
      msg.msg_controllen= 0;
      msg.msg_flags= 0;

      len= 0;
      mbody[len++]= 'V';
      mbody[len++]= 'S';
      mbody[len++]= 1;
      mbody[len++]= 'S';

      iov[0].iov_base= (char*)mbody;
      iov[0].iov_len= len;

      do
      {
         sentLen= sendmsg( conn->socketFd, &msg, MSG_NOSIGNAL );
      }
      while ( (sentLen < 0) && (errno == EINTR));

      if ( sentLen == len )
      {
         GST_LOG("sent flush to video server");
         FRAME("sent flush to video server");
      }
   }
}

static void wstProcessMessagesVideoClientConnection( WstVideoClientConnection *conn )
{
   if ( conn )
   {
      GstWesterosSink *sink= conn->sink;
      struct pollfd pfd;
      int rc;

      pfd.fd= conn->socketFd;
      pfd.events= POLLIN;
      pfd.revents= 0;

      rc= poll( &pfd, 1, 0);
      if ( rc == 1 )
      {
         struct msghdr msg;
         struct iovec iov[1];
         unsigned char mbody[64];
         unsigned char *m= mbody;
         int len;

         iov[0].iov_base= (char*)mbody;
         iov[0].iov_len= sizeof(mbody);

         msg.msg_name= NULL;
         msg.msg_namelen= 0;
         msg.msg_iov= iov;
         msg.msg_iovlen= 1;
         msg.msg_control= 0;
         msg.msg_controllen= 0;
         msg.msg_flags= 0;

         do
         {
            len= recvmsg( conn->socketFd, &msg, 0 );
         }
         while ( (len < 0) && (errno == EINTR));

         while ( len >= 4 )
         {
            if ( (m[0] == 'V') && (m[1] == 'S') )
            {
               int mlen, id;
               mlen= m[2];
               if ( len >= (mlen+3) )
               {
                  id= m[3];
                  switch( id )
                  {
                     case 'R':
                        if ( mlen >= 5)
                        {
                          int rate= getU32( &m[4] );
                          GST_DEBUG("got rate %d from video server", rate);
                          conn->serverRefreshRate= rate;
                          if ( rate )
                          {
                             conn->serverRefreshPeriod= 1000000LL/rate;
                          }
                          FRAME("got rate %d (period %lld us) from video server", rate, conn->serverRefreshPeriod);
                        }
                        break;
                     case 'B':
                        if ( mlen >= 5)
                        {
                          int bi= getU32( &m[4] );
                          if ( sink->soc.drmBuffer[bi].locked )
                          {
                             FRAME("out:       release received for buffer %d (%d)", bi, bi);
                             if ( drmUnlockBuffer( sink, bi ) )
                             {
                                drmReleaseBuffer( sink, bi );
                             }
                          }
                          else
                          {
                             GST_ERROR("release received for non-locked buffer %d (%d)", bi, bi );
                             FRAME("out:       error: release received for non-locked buffer %d (%d)", bi, bi);
                          }
                        }
                        break;
                     case 'S':
                        if ( mlen >= 13)
                        {
                           /* set position from frame currently presented by the video server */
                           guint64 frameTime= getS64( &m[4] );
                           sink->soc.numDropped= getU32( &m[12] );
                           FRAME( "out:       status received: frameTime %lld numDropped %d", frameTime, sink->soc.numDropped);
                           gint64 currentNano= frameTime*1000LL;
                           gint64 firstNano= ((sink->firstPTS/90LL)*GST_MSECOND)+((sink->firstPTS%90LL)*GST_MSECOND/90LL);
                           sink->position= sink->positionSegmentStart + currentNano - firstNano;
                           sink->currentPTS= currentNano / (GST_SECOND/90000LL);
                           GST_LOG("receive frameTime: %lld position %lld", currentNano, sink->position);
                           if (sink->soc.frameDisplayCount == 0)
                           {
                               sink->soc.emitFirstFrameSignal= TRUE;
                           }
                           ++sink->soc.frameDisplayCount;
                           if ( sink->timeCodePresent && sink->enableTimeCodeSignal )
                           {
                              sink->timeCodePresent( sink, sink->position, g_signals[SIGNAL_TIMECODE] );
                           }
                        }
                        break;
                     default:
                        break;
                  }
                  m += (mlen+3);
                  len -= (mlen+3);
               }
               else
               {
                  len= 0;
               }
            }
            else
            {
               len= 0;
            }
         }
      }
   }
}

static bool wstSendFrameVideoClientConnection( WstVideoClientConnection *conn, int buffIndex )
{
   bool result= false;
   GstWesterosSink *sink= conn->sink;
   int sentLen;

   if ( conn  )
   {
      struct msghdr msg;
      struct cmsghdr *cmsg;
      struct iovec iov[1];
      unsigned char mbody[4+64];
      char cmbody[CMSG_SPACE(3*sizeof(int))];
      int i, len;
      int *fd;
      int numFdToSend;
      int frameFd0= -1, frameFd1= -1, frameFd2= -1;
      int fdToSend0= -1, fdToSend1= -1, fdToSend2= -1;
      int offset0, offset1, offset2;
      int stride0, stride1, stride2;
      uint32_t pixelFormat;
      int bufferId= -1;
      int vx, vy, vw, vh;

      wstProcessMessagesVideoClientConnection( conn );

      if ( buffIndex >= 0 )
      {
         sink->soc.resubFd= -1;

         bufferId= sink->soc.drmBuffer[buffIndex].bufferId;

         numFdToSend= 1;
         offset0= offset1= offset2= 0;
         stride0= stride1= stride2= sink->soc.frameWidth;
         frameFd0= sink->soc.drmBuffer[buffIndex].fd0;
         stride0= sink->soc.drmBuffer[buffIndex].pitch0;
         frameFd1= sink->soc.drmBuffer[buffIndex].fd1;
         stride1= sink->soc.drmBuffer[buffIndex].pitch1;

         pixelFormat= DRM_FORMAT_NV12;

         fdToSend0= fcntl( frameFd0, F_DUPFD_CLOEXEC, 0 );
         if ( fdToSend0 < 0 )
         {
            GST_ERROR("wstSendFrameVideoClientConnection: failed to dup fd0");
            goto exit;
         }
         if ( frameFd1 >= 0 )
         {
            fdToSend1= fcntl( frameFd1, F_DUPFD_CLOEXEC, 0 );
            if ( fdToSend1 < 0 )
            {
               GST_ERROR("wstSendFrameVideoClientConnection: failed to dup fd1");
               goto exit;
            }
            ++numFdToSend;
         }
         if ( frameFd2 >= 0 )
         {
            fdToSend2= fcntl( frameFd2, F_DUPFD_CLOEXEC, 0 );
            if ( fdToSend2 < 0 )
            {
               GST_ERROR("wstSendFrameVideoClientConnection: failed to dup fd2");
               goto exit;
            }
            ++numFdToSend;
         }

         vx= sink->soc.videoX;
         vy= sink->soc.videoY;
         vw= sink->soc.videoWidth;
         vh= sink->soc.videoHeight;
         if ( sink->soc.forceAspectRatio )
         {
            wstGetVideoBounds( sink, &vx, &vy, &vw, &vh );
         }

         i= 0;
         mbody[i++]= 'V';
         mbody[i++]= 'S';
         mbody[i++]= 65;
         mbody[i++]= 'F';
         i += putU32( &mbody[i], conn->sink->soc.frameWidth );
         i += putU32( &mbody[i], conn->sink->soc.frameHeight );
         i += putU32( &mbody[i], pixelFormat );
         i += putU32( &mbody[i], vx );
         i += putU32( &mbody[i], vy );
         i += putU32( &mbody[i], vw );
         i += putU32( &mbody[i], vh );
         i += putU32( &mbody[i], offset0 );
         i += putU32( &mbody[i], stride0 );
         i += putU32( &mbody[i], offset1 );
         i += putU32( &mbody[i], stride1 );
         i += putU32( &mbody[i], offset2 );
         i += putU32( &mbody[i], stride2 );
         i += putU32( &mbody[i], bufferId );
         i += putS64( &mbody[i], sink->soc.drmBuffer[buffIndex].frameTime );

         iov[0].iov_base= (char*)mbody;
         iov[0].iov_len= i;

         cmsg= (struct cmsghdr*)cmbody;
         cmsg->cmsg_len= CMSG_LEN(numFdToSend*sizeof(int));
         cmsg->cmsg_level= SOL_SOCKET;
         cmsg->cmsg_type= SCM_RIGHTS;

         msg.msg_name= NULL;
         msg.msg_namelen= 0;
         msg.msg_iov= iov;
         msg.msg_iovlen= 1;
         msg.msg_control= cmsg;
         msg.msg_controllen= cmsg->cmsg_len;
         msg.msg_flags= 0;

         fd= (int*)CMSG_DATA(cmsg);
         fd[0]= fdToSend0;
         if ( fdToSend1 >= 0 )
         {
            fd[1]= fdToSend1;
         }
         if ( fdToSend2 >= 0 )
         {
            fd[2]= fdToSend2;
         }
         GST_LOG( "%lld: send frame: %d, fd (%d, %d, %d [%d, %d, %d])", getCurrentTimeMillis(), buffIndex, frameFd0, frameFd1, frameFd2, fdToSend0, fdToSend1, fdToSend2);
         drmLockBuffer( sink, buffIndex );
         FRAME("out:       send frame %d buffer %d (%d)", conn->sink->soc.frameOutCount, conn->sink->soc.drmBuffer[buffIndex].bufferId, buffIndex);

         do
         {
            sentLen= sendmsg( conn->socketFd, &msg, 0 );
         }
         while ( (sentLen < 0) && (errno == EINTR));

         conn->sink->soc.drmBuffer[buffIndex].frameNumber= conn->sink->soc.frameOutCount;

         if ( sentLen == iov[0].iov_len )
         {
            result= true;
         }
         else
         {
            FRAME("out:       failed send frame %d buffer %d (%d)", conn->sink->soc.frameOutCount, conn->sink->soc.drmBuffer[buffIndex].bufferId, buffIndex);
            if ( drmUnlockBuffer( sink, buffIndex ) )
            {
               drmReleaseBuffer( sink, buffIndex );
            }
         }
      }

exit:
      if ( fdToSend0 >= 0 )
      {
         close( fdToSend0 );
      }
      if ( fdToSend1 >= 0 )
      {
         close( fdToSend1 );
      }
      if ( fdToSend2 >= 0 )
      {
         close( fdToSend2 );
      }
   }
   return result;
}

static gpointer wstDispatchThread(gpointer data)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;
   if ( sink->display )
   {
      GST_DEBUG("dispatchThread: enter");
      while( !sink->soc.quitDispatchThread )
      {
         if ( wl_display_dispatch_queue( sink->display, sink->queue ) == -1 )
         {
            break;
         }
      }
      GST_DEBUG("dispatchThread: exit");
   }
   return NULL;
}

static gpointer wstEOSDetectionThread(gpointer data)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;
   int outputFrameCount, count, eosCountDown;
   bool videoPlaying;
   bool eosEventSeen;
   double frameRate;

   GST_DEBUG("wstVideoEOSThread: enter");

   eosCountDown= 10;
   LOCK(sink)
   outputFrameCount= sink->soc.frameOutCount;
   frameRate= (sink->soc.frameRate > 0.0 ? sink->soc.frameRate : 30.0);
   UNLOCK(sink);
   while( !sink->soc.quitEOSDetectionThread )
   {
      usleep( 1000000/frameRate );

      if ( !sink->soc.quitEOSDetectionThread )
      {
         LOCK(sink)
         count= sink->soc.frameOutCount;
         videoPlaying= sink->soc.videoPlaying;
         eosEventSeen= sink->eosEventSeen;
         UNLOCK(sink)

         if ( videoPlaying && eosEventSeen && (outputFrameCount > 0) && (outputFrameCount == count) )
         {
            --eosCountDown;
            if ( eosCountDown == 0 )
            {
               g_print("westeros-sink: EOS detected\n");
               gst_element_post_message (GST_ELEMENT_CAST(sink), gst_message_new_eos(GST_OBJECT_CAST(sink)));
               break;
            }
         }
         else
         {
            outputFrameCount= count;
            eosCountDown= 10;
         }
      }
   }

   if ( !sink->soc.quitEOSDetectionThread )
   {
      GThread *thread= sink->soc.eosDetectionThread;
      g_thread_unref( sink->soc.eosDetectionThread );
      sink->soc.eosDetectionThread= NULL;
   }

   GST_DEBUG("wstVideoEOSThread: exit");

   return NULL;
}

static gpointer wstFirstFrameThread(gpointer data)
{
   GstWesterosSink *sink= (GstWesterosSink*)data;

   if ( sink )
   {
      GST_DEBUG("wstFirstFrameThread: emit first frame signal");
      g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_FIRSTFRAME], 0, 2, NULL);
      g_thread_unref( sink->soc.firstFrameThread );
      sink->soc.firstFrameThread= NULL;
   }

   return NULL;
}

#define DEFAULT_DRM_NAME "/dev/dri/card0"

static bool drmInit( GstWesterosSink *sink )
{
   const char *drmName;

   drmName= getenv("WESTEROS_SINK_DRM_NAME");
   if ( !drmName )
   {
      drmName= DEFAULT_DRM_NAME;
   }

   GST_DEBUG("drmInit");
   sink->soc.drmFd= open( drmName, O_RDWR );
   if ( sink->soc.drmFd < 0 )
   {
      GST_ERROR("Failed to open drm node (%s): %d", drmName, errno);
      goto exit;
   }

exit:
   return true;
}

static void drmTerm( GstWesterosSink *sink )
{
   int i;
   GST_DEBUG("drmTerm");
   if ( sink->soc.eosDetectionThread )
   {
      sink->soc.quitEOSDetectionThread= TRUE;
      g_thread_join( sink->soc.eosDetectionThread );
      sink->soc.eosDetectionThread= NULL;
   }
   if ( sink->soc.dispatchThread )
   {
      sink->soc.quitDispatchThread= TRUE;
      g_thread_join( sink->soc.dispatchThread );
      sink->soc.dispatchThread= NULL;
   }
   for( i= 0; i < WST_NUM_DRM_BUFFERS; ++i )
   {
      drmFreeBuffer( sink, i );
   }
   if ( sink->soc.drmFd >= 0 )
   {
      close( sink->soc.drmFd );
      sink->soc.drmFd= 1;
   }
}

static bool drmAllocBuffer( GstWesterosSink *sink, int buffIndex, int width, int height )
{
   bool result= false;
   WstDrmBuffer *drmBuff= 0;
   if ( buffIndex < WST_NUM_DRM_BUFFERS )
   {
      struct drm_mode_create_dumb createDumb;
      struct drm_mode_map_dumb mapDumb;
      int i, rc;

      drmBuff= &sink->soc.drmBuffer[buffIndex];

      drmBuff->width= width;
      drmBuff->height= height;
      GST_LOG("drmAllocBuffer: (%dx%d)", width, height);

      width= ((width+63)&~63);

      memset( &createDumb, 0, sizeof(createDumb) );
      createDumb.width= width;
      createDumb.height= height;
      createDumb.bpp= 8;
      rc= ioctl( sink->soc.drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &createDumb );
      if ( rc )
      {
         GST_ERROR("DRM_IOCTL_MODE_CREATE_DUMB failed: rc %d errno %d", rc, errno);
         goto exit;
      }
      memset( &mapDumb, 0, sizeof(mapDumb) );
      mapDumb.handle= createDumb.handle;
      rc= ioctl( sink->soc.drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapDumb );
      if ( rc )
      {
         GST_ERROR("DRM_IOCTL_MODE_MAP_DUMB failed: rc %d errno %d", rc, errno);
         goto exit;
      }
      drmBuff->handle0= createDumb.handle;
      drmBuff->pitch0= createDumb.pitch;
      drmBuff->size0= createDumb.size;
      drmBuff->offset0= mapDumb.offset;

      rc= drmPrimeHandleToFD( sink->soc.drmFd, drmBuff->handle0, DRM_CLOEXEC | DRM_RDWR, &drmBuff->fd0 );
      if ( rc )
      {
         GST_ERROR("drmPrimeHandleToFD failed: rc %d errno %d", rc, errno);
         goto exit;
      }

      memset( &createDumb, 0, sizeof(createDumb) );
      createDumb.width= width;
      createDumb.height= height/2;
      createDumb.bpp= 8;
      rc= ioctl( sink->soc.drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &createDumb );
      if ( rc )
      {
         GST_ERROR("DRM_IOCTL_MODE_CREATE_DUMB failed: rc %d errno %d\n", rc, errno);
         goto exit;
      }
      memset( &mapDumb, 0, sizeof(mapDumb) );
      mapDumb.handle= createDumb.handle;
      rc= ioctl( sink->soc.drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapDumb );
      if ( rc )
      {
         GST_ERROR("DRM_IOCTL_MODE_MAP_DUMB failed: rc %d errno %d", rc, errno);
         goto exit;
      }
      drmBuff->handle1= createDumb.handle;
      drmBuff->pitch1= createDumb.pitch;
      drmBuff->size1= createDumb.size;
      drmBuff->offset1= mapDumb.offset;

      rc= drmPrimeHandleToFD( sink->soc.drmFd, drmBuff->handle1, DRM_CLOEXEC | DRM_RDWR, &drmBuff->fd1 );
      if ( rc )
      {
         GST_ERROR("drmPrimeHandleToFD failed: rc %d errno %d", rc, errno);
         goto exit;
      }

      drmBuff->bufferId= buffIndex;

      result= true;
   }
exit:
   if ( !result )
   {
      drmFreeBuffer( sink, buffIndex );
   }
   return result;
}

static void drmFreeBuffer( GstWesterosSink *sink, int buffIndex )
{
   int i;
   if (
        (buffIndex < WST_NUM_DRM_BUFFERS) &&
        (sink->soc.drmBuffer[buffIndex].width != -1) &&
        (sink->soc.drmBuffer[buffIndex].height != -1)
      )
   {
      GST_LOG("drmFreeBuffer: (%dx%d)", sink->soc.drmBuffer[buffIndex].width, sink->soc.drmBuffer[buffIndex].height);
   }
   for( i= 0; i < 2; ++i )
   {
      int *fd, *handle;
      if ( i == 0 )
      {
         fd= &sink->soc.drmBuffer[buffIndex].fd0;
         handle= &sink->soc.drmBuffer[buffIndex].handle0;
      }
      else
      {
         fd= &sink->soc.drmBuffer[buffIndex].fd1;
         handle= &sink->soc.drmBuffer[buffIndex].handle1;
      }
      if ( *fd >= 0 )
      {
         close( *fd );
         *fd= -1;
      }
      if ( *handle )
      {
         struct drm_mode_destroy_dumb destroyDumb;
         destroyDumb.handle= *handle;
         ioctl( sink->soc.drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyDumb );
         *handle= 0;
      }
   }
}

static void drmLockBuffer( GstWesterosSink *sink, int buffIndex )
{
   sink->soc.drmBuffer[buffIndex].locked= true;
   ++sink->soc.drmBuffer[buffIndex].lockCount;
}

static bool drmUnlockBuffer( GstWesterosSink *sink, int buffIndex )
{
   bool unlocked= false;
   if ( !sink->soc.drmBuffer[buffIndex].locked )
   {
      GST_ERROR("attempt to unlock buffer that is not locked: index %d", buffIndex);
   }
   if ( sink->soc.drmBuffer[buffIndex].lockCount > 0 )
   {
      if ( --sink->soc.drmBuffer[buffIndex].lockCount == 0 )
      {
         sink->soc.drmBuffer[buffIndex].locked= false;
         unlocked= true;
      }
   }
   return unlocked;
}

static bool drmUnlockAllBuffers( GstWesterosSink *sink )
{
   WstDrmBuffer *drmBuff= 0;
   int buffIndex;
   for( buffIndex= 0; buffIndex < WST_NUM_DRM_BUFFERS; ++buffIndex )
   {
      drmBuff= &sink->soc.drmBuffer[buffIndex];
      if ( drmBuff->locked )
      {
         drmBuff->locked= false;
         drmBuff->lockCount= 0;
      }
   }
   sem_post( &sink->soc.drmBuffSem );
}

static WstDrmBuffer *drmGetBuffer( GstWesterosSink *sink, int width, int height )
{
   WstDrmBuffer *drmBuff= 0;
   int buffIndex;
   int rc;

   for ( ; ; )
   {
      rc= sem_trywait( &sink->soc.drmBuffSem );
      if ( rc )
      {
         if ( errno == EAGAIN )
         {
            usleep( 1000 );
            wstProcessMessagesVideoClientConnection( sink->soc.conn );
            continue;
         }
      }
      break;
   }

   for( buffIndex= 0; buffIndex < WST_NUM_DRM_BUFFERS; ++buffIndex )
   {
      drmBuff= &sink->soc.drmBuffer[buffIndex];
      if ( !drmBuff->locked )
      {
         if ( (drmBuff->width != width) || (drmBuff->height != height) )
         {
            drmFreeBuffer( sink, buffIndex );
            if ( !drmAllocBuffer( sink, buffIndex, width, height ) )
            {
               drmBuff= 0;
            }
         }
         break;
      }
      else
      {
         drmBuff= 0;
      }
   }
   return drmBuff;
}

static void drmReleaseBuffer( GstWesterosSink *sink, int buffIndex )
{
   if ( !sink->soc.drmBuffer[buffIndex].locked )
   {
      int rc;
      sink->soc.drmBuffer[buffIndex].frameNumber= -1;
      FRAME("out:       release buffer %d (%d)", sink->soc.drmBuffer[buffIndex].bufferId, buffIndex);
      GST_LOG( "%lld: release: buffer %d (%d)", getCurrentTimeMillis(), sink->soc.drmBuffer[buffIndex].bufferId, buffIndex);
      sem_post( &sink->soc.drmBuffSem );
   }
}

