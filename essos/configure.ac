#
# If not stated otherwise in this file or this component's Licenses.txt file the
# following copyright and licenses apply:
#
# Copyright 2017-2018 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

dnl AC_PREREQ([2.65])
AC_INIT([essos], [1.0.0], [jeff_wannamaker@cable.comcast.com,essos])
AC_CONFIG_SRCDIR([essos.h])
AC_CONFIG_MACRO_DIRS([.m4])
AC_CONFIG_AUX_DIR([.automake])
AM_INIT_AUTOMAKE([foreign no-dist-gzip dist-bzip2 1.9])
LT_INIT

# Checks for programs.
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_CXX

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h \
                  string.h \
                  unistd.h \
                  linux/input.h \
                  sys/mman.h \
                  sys/inotify.h \
                  wayland-client.h \
                  wayland-egl.h \
                  xkbcommon/xkbcommon.h \
                  westeros-gl.h])

AC_CHECK_HEADER([EGL/egl.h], [], [AC_MSG_ERROR([Unable to find EGL/eg.h])], [])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_HEADER_STDBOOL

# It would be nice to have a 'westeros-gl.pc' pkgconf(1) compatible file, as of now
# behave like we would have 'westeros-gl.pc' file, which can be queried by PKG_CHECK_MODULES
# so from the rest of the code that becomes fully transparent.
AC_CHECK_HEADER([westeros-gl.h], [westeros_found=yes])
AS_IF([test x"$westeros_found" == x"yes"],
      [AC_DEFINE([HAVE_WESTEROS_GL_H], [1], [Define to 1 if you have <westeros-gl.h> header file.])
       WESTEROS_CFLAGS=""
       WESTEROS_LIBS="-lwesteros-gl"],
      [])
AC_SUBST(WESTEROS_CFLAGS)
AC_SUBST(WESTEROS_LIBS)

PKG_CHECK_MODULES([EGL], [egl >= 1.0.0])
AC_SUBST(EGL_CFLAGS)
AC_SUBST(EGL_LIBS)

PKG_CHECK_MODULES([WAYLAND], [wayland-client >= 1.6.0])
AC_SUBST(WAYLAND_CFLAGS)
AC_SUBST(WAYLAND_LIBS)

PKG_CHECK_MODULES([WAYLAND_EGL], [wayland-egl >= 1.0.0])
AC_SUBST(WAYLAND_EGL_CFLAGS)
AC_SUBST(WAYLAND_EGL_LIBS)

PKG_CHECK_MODULES([XKBCOMMON], [xkbcommon >= 0.5.0])
AC_SUBST(XKBCOMMON_CFLAGS)
AC_SUBST(XKBCOMMON_LIBS)

# For sample application
PKG_CHECK_MODULES([GLESv2], [glesv2 >= 1.0.0])
AC_SUBST(GLESv2_CFLAGS)
AC_SUBST(GLESv2_LIBS)

AC_CONFIG_FILES([Makefile essos.pc])
AC_OUTPUT()
