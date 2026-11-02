#!/bin/bash
# Regenerate, rebuild the host harness and boot it for $1 virtual seconds.
set -e
cd "$(dirname "$0")"
make gen | tail -1
rm -f build/host/xlat_*.o; make -j16 host 2>&1 | grep -E "error|undefined" | cut -c1-220 | head -20 || true
timeout ${2:-120} ./build/archicast-host build/rom.bin assets/cmos.bin ${1:-30} build/shot.ppm 2>&1 | tail -9
