#!/bin/bash
# Build fieldtag for the Pager (FPU-less mipsel, freestanding, no libc).
# Toolchain: brew install messense/macos-cross-toolchains/mipsel-unknown-linux-gnu
set -e
cd "$(dirname "$0")"
CC="${CC:-mipsel-unknown-linux-gnu-gcc}"
"$CC" -O2 -fno-pic -mno-abicalls -G0 -nostdlib -ffreestanding -static -s \
    -Wl,-e,_start -o fieldtag fieldtag.c
file fieldtag
ls -lh fieldtag | awk '{print "size:", $5}'
shasum -a 256 fieldtag
# host preview (for UI tuning): cc -DHOST_PREVIEW -O2 -o fieldtag_host fieldtag.c
