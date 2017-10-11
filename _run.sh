#!/bin/bash
export LD_LIBRARY_PATH=$PWD/out/lib
$ENTRYPOINT out/bin/westeros --renderer libwesteros_render_gl.so.0 --framerate 60 --display wayland-1 "$@"
