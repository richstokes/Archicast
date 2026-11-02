#!/bin/bash
# Boot the harness many times with randomised clocks; report anything off the desktop.
cd "$(dirname "$0")/.."
one() {
  out=$(DREAM_US_STEP=$1 DREAM_US_JITTER=$2 DREAM_SEED=$3 timeout 60 ./build/archicast-host build/rom.bin assets/cmos.bin 45 /dev/null 2>&1)
  echo "$1 $2 $3 $(echo "$out" | grep -E 'screen sum|no native' | head -1) | $(echo "$out" | grep '^IOC' | cut -c1-60)"
}
export -f one
for step in 2 4 8 16; do for j in 0 8 40 200 2000; do for seed in 1 2 3 4 5 6 7 8; do echo "$step $j $seed"; done; done; done | xargs -P 14 -L 1 bash -c 'one $0 $1 $2'
