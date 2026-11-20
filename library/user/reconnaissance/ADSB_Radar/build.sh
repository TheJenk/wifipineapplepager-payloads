#!/bin/bash
# Build the `radar` binary for the Pager (MT7628, MIPS32r2 little-endian, no FPU).
# Freestanding: no libc, integer-only (the SoC has no FPU and the kernel won't emulate it).
#   ./build.sh                     -> ./radar
#   CC=/path/to/mipsel-gcc ./build.sh
# Host preview (Mac/Linux): renders frames from a raw .cu8 recording to PPM files:
#   cc -O2 -DHOST_PREVIEW -o radar_host src/radar.c
#   ADSB_MAP=map.bin ./radar_host capture.cu8 outdir [mode lat_e6 lon_e6] [keys]
set -e
cd "$(dirname "$0")"
CC="${CC:-mipsel-unknown-linux-gnu-gcc}"
"$CC" -DPAGER -O2 -march=mips32r2 -msoft-float -fno-pic -mno-abicalls -G0 \
  -nostdlib -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -static -s -Wl,-e,_start \
  -o radar src/radar.c -lgcc 2>&1 | grep -v -E "warning: (linking abicalls|.* uses -m(soft|hard)-float)" || true
ls -l radar | awk '{print "radar:", $5, "bytes"}'
