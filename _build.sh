#!/bin/bash -e

export LT_SYS_LIBRARY_PATH=$PWD/out/lib
export LDFLAGS=${LT_SYS_LIBRARY_PATH}

export SCANNER_TOOL=wayland-scanner

export CFLAGS="-O0 -g3 -I$PWD/out/include"
export CXXFLAGS="${CFLAGS}"
export LDFLAGS="${CFLAGS} -L${PWD}/out/lib"

make -C simpleshell/protocol
ln -sf simpleshell/westeros-simpleshell.h
ln -sf simpleshell/protocol/simpleshell-client-protocol.h
ln -sf simpleshell/protocol/simpleshell-server-protocol.h
(cd simpleshell; autoreconf -sif; ./configure --prefix=$PWD/../out; make; make install)


make -C simplebuffer/protocol
ln -sf simplebuffer/westeros-simplebuffer.h
ln -sf simplebuffer/protocol/simplebuffer-client-protocol.h
ln -sf simplebuffer/protocol/simplebuffer-server-protocol.h
(cd simplebuffer; autoreconf -sif; ./configure --prefix=$PWD/../out; make; make install)


make -C protocol
ln -sf protocol/version5/xdg-shell-server-protocol.h

(cd drm; autoreconf -sif; ./configure --prefix=$PWD/../out; make; make install)

autoreconf -sif

./configure \
    --prefix=$PWD/out \
    --enable-rendergl=yes \
    --enable-sbprotocol=yes \
    --enable-app=yes \
    --disable-xdgv4 \
    --enable-xdgv5 \
    --enable-test \
    

make -j $(nproc)

make install
