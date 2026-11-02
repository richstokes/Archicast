#!/bin/bash
# Replay a script against the harness, turning each "no native code" stop into
# an entry in trace/seeds.txt or trace/ramwords.txt, until the replay completes.
# usage: tools/converge.sh <replay> <seconds> [iterations]
cd "$(dirname "$0")/.."
replay=$1; seconds=$2; limit=${3:-40}
export ROM_DIR=${ROM_DIR:-../arculator-mac/roms/riscos311}
for ((i = 0; i < limit; i++)); do
  make gen >/dev/null && rm -f build/host/xlat_*.o && make -j16 host 2>&1 | grep -E " error" | head -3
  out=$(DREAM_REPLAY=$replay ./build/archicast-host build/rom.bin assets/cmos.bin "$seconds" build/shot.ppm 2>&1)
  miss=$(echo "$out" | grep -o 'no native code for pc=[0-9A-F]* word=[0-9A-F]*' | head -1)
  if [ -z "$miss" ]; then echo "$out" | tail -1; echo "converged after $i misses"; exit 0; fi
  pc=${miss#*pc=}; pc=${pc%% *}; word=${miss##*word=}
  echo "$miss"
  if ((16#$pc >= 0x3800000)); then echo "$pc" >> trace/seeds.txt; else
    # Take the whole routine around the miss, skipping text and table data.
    python3 - "$word" <<'PY' >> trace/ramwords.txt
import sys
words = {int(sys.argv[1], 16)}
for line in open("build/miss.txt"):
    w = int(line, 16)
    if w >> 28 != 15 and w > 0xFFFF and w >> 24 not in range(0x20, 0x7F):
        words.add(w)
print("\n".join(f"{w:08X}" for w in sorted(words)))
PY
  fi
done
echo "gave up after $limit iterations"; exit 1
