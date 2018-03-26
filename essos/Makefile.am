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

ACLOCAL_AMFLAGS = -I .m4
AUTOMAKE_OPTIONS = subdir-objects

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = essos.pc

lib_LTLIBRARIES = libessos.la

bin_PROGRAMS = essos-sample

BUILT_SOURCES= libessos.la

libessos_la_SOURCES = essos.cpp
libessos_la_include_HEADERS = essos.h
libessos_la_includedir = $(includedir)

libessos_la_CXXFLAGS =
                     #  version-info current:revision:age
libessos_la_LDFLAGS  = -version-info 1:0:0 $(AM_LDFLAGS) \
              $(EGL_LIBS)   $(WESTEROS_LIBS)   $(WAYLAND_LIBS)   $(WAYLAND_EGL_LIBS)   $(XKBCOMMON_LIBS)
AM_CPPFLAGS = $(EGL_CFLAGS) $(WESTEROS_CFLAGS) $(WAYLAND_CFLAGS) $(WAYLAND_EGL_CFLAGS) $(XKBCOMMON_CFLAGS)

essos_sample_SOURCES  = essos-sample.cpp
essos_sample_CXXFLAGS = $(GLESv2_CFLAGS)
essos_sample_LDFLAGS  = $(AM_FLAGS) $(GLESv2_LIBS) -lessos

EXTRA_DIST = README.md LICENSE

MAINTAINERCLEANFILES = essos.pc Makefile Makefile.in aclocal.m4 config.log \
                       config.status configure essos.pc libtool

maintainer-clean-local:
	-rm -rf .automake .m4

## IPK Generation Support
IPK_GEN_PATH = $(abs_top_builddir)/ipk
IPK_GEN_STAGING_DIR=$(abs_top_builddir)/staging_dir
IPK_GEN_INPUT = debian-binary control.tar.gz data.tar.gz
IPK_GEN_OUTPUT = rcmodule.ipk

package_ipk:
	@echo "Generating IPK Package"
	$(MAKE) prefix=${abs_top_builddir}/staging_dir install
	$(mkdir_p) $(IPK_GEN_STAGING_DIR)/usr/bin/
	cp -af $(IPK_GEN_STAGING_DIR)/bin/* $(IPK_GEN_STAGING_DIR)/usr/bin/
	tar -czvf $(IPK_GEN_PATH)/data.tar.gz -C $(IPK_GEN_STAGING_DIR) usr
	tar -czvf $(IPK_GEN_PATH)/control.tar.gz -C $(IPK_GEN_PATH) control postinst prerm
	cd $(IPK_GEN_PATH) && ar cr $(IPK_GEN_OUTPUT) $(IPK_GEN_INPUT) && cd -
	rm -rf ${abs_top_builddir}/staging_dir
	rm -rf $(abs_top_builddir)/ipk/control.tar.gz
	rm -rf $(abs_top_builddir)/ipk/data.tar.gz
	
