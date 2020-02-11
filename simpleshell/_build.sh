#!/bin/sh

# dnf install make autoconf automake libtool glib2-devel wayland-devel gcc-c++

make -C protocol SCANNER_TOOL=wayland-scanner

autoreconf -si && ./configure --prefix=$PWD/out && make -j$(getconf _NPROCESSORS_ONLN) install

