#!/bin/bash
# Cross-compile the WiFi Radar binary for the Pager (FPU-less mipsel, freestanding).
# Toolchain: brew install messense/macos-cross-toolchains/mipsel-unknown-linux-gnu
set -e
CC="${CC:-mipsel-unknown-linux-gnu-gcc}"
"$CC" -O2 -fno-pic -mno-abicalls -G0 -nostdlib -ffreestanding -static -s \
      -Wl,-e,_start -o radar radar.c
file radar; ls -lh radar | awk '{print "size:",$5}'
if "${CC%gcc}objdump" -d radar 2>/dev/null | grep -qiE '\$f[0-9]|lwc1|swc1|cvt\.'; then
    echo "WARNING: floating-point instructions present - will SIGILL on device!" >&2
fi
# Host preview: cc -O2 -DHOST_PREVIEW -o radar_host radar.c && ./radar_host --preview /tmp/out
