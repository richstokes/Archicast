#!/bin/bash
# Build an image that launches Lander and flies a script, reporting game frames
# (flips) in heartbeats. Run gen first. Writes build/archicast-hw.elf and
# dist/Archicast-hw.cdi, and leaves the normal main.o to be rebuilt.
# Then: dc-hw.sh run -c -o -T 170 -l build/hw-lander.log -u 'hb t=13[0-9]' build/archicast-hw.elf
# usage: scripts/bench-lander.sh [seconds to subtract from replay times] [extra make args]
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
python3 tools/replay2c.py tests/lander.replay "${1:--40}" > build/replay.h
rm -f build/dc/main.o build/dc/mach.o
DC_MAKE_ARGS="HW_LOG=1 HW_REPLAY=1 ${2:-}" DC_CDI=dist/Archicast-hw.cdi scripts/build-dc.sh --skip-gen
cp build/archicast.elf build/archicast-hw.elf
rm -f build/dc/main.o build/dc/mach.o build/archicast.elf
