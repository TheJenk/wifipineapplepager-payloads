#!/bin/bash
# Cross-compile the Wardrive HUD binary (FPU-less mipsel, freestanding).
set -e
CC="${CC:-mipsel-unknown-linux-gnu-gcc}"
"$CC" -O2 -fno-pic -mno-abicalls -G0 -nostdlib -ffreestanding -static -s -Wl,-e,_start -o wardrive wardrive.c
file wardrive; ls -lh wardrive | awk '{print "size:",$5}'
# Host preview (no hardware needed - uses synthetic GPS data):
#   cc -O2 -DHOST_PREVIEW -o wardrive_host wardrive.c && ./wardrive_host --preview /tmp/out
