#!/bin/bash -ex
#
# Simple build script
# Author: Damian Wrobel <dwrobel@ertelnet.rybnik.pl>
#
# Fedora dependencies:
# dnf install make autoconf automake libtool glib2-devel wayland-devel mesa-libgbm-devel egl-wayland-devel libdrm-devel libglvnd-devel libxkbcommon-devel gstreamer1-devel mesa-dri-drivers gcc-c++
#

export OUTDIR=$PWD/out

export LT_SYS_LIBRARY_PATH=${OUTDIR}/lib
export LDFLAGS=${LT_SYS_LIBRARY_PATH}

export SCANNER_TOOL=wayland-scanner

export CFLAGS="-O0 -g3 -I${OUTDIR}/include"
export CXXFLAGS="${CFLAGS}"
export LDFLAGS="${CFLAGS} -L${OUTDIR}/lib"

for dir in simpleshell simplebuffer .; do
   make -C ${dir}/protocol
done

for dir in simpleshell simplebuffer drm; do
  pushd ${dir}
    autoreconf -sif; ./configure --prefix=${OUTDIR}; make -j$(getconf _NPROCESSORS_ONLN) V=1 install
  popd
done

autoreconf -sif

./configure \
    --prefix=${OUTDIR} \
    --enable-rendergl \
    --enable-sbprotocol \
    --enable-app \
    --disable-xdgv4 \
    --disable-xdgv5 \
    --enable-xdgstable \
    --enable-test \
    --enable-modules \
    

make -j$(getconf _NPROCESSORS_ONLN) V=1 install
