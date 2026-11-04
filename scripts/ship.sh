#!/bin/bash
# Verify the tree and push to main. Refuses on conflict markers, a failed build
# or a replay that no longer matches. Commit first; this only checks and pushes.
set -euo pipefail
cd "$(dirname "$0")/.."
if grep -rnE '^(<<<<<<<|>>>>>>>) ' src tools/*.py tools/*.sh docs README.md scripts Makefile trace/seeds.txt trace/ramwords.txt tests/expected.txt; then
  echo "conflict markers present" >&2; exit 1
fi
[ -z "$(git status --porcelain --ignored=no -- . ':!.DS_Store')" ] || { echo "uncommitted changes" >&2; exit 1; }
git fetch -q origin
git rebase origin/main
make gen | tail -1
rm -rf build/host build/dc build/archicast-host
make -j16 host 2>&1 | { ! grep -E " error|Undefined"; }
make interp-test | tail -1
scripts/build-dc.sh --skip-gen > build/ship.log 2>&1 || { tail -5 build/ship.log; exit 1; }
check() {
  gunzip -c assets/adfs.adf.gz > build/test.adf
  rm -f build/test-hd.img
  got=$(env ${4:+DREAM_ADF=$4} ${5:+DREAM_HD=$5} DREAM_REPLAY=$1 ./build/archicast-host build/rom.bin assets/cmos.bin "$2" /dev/null 2>&1 | grep -oE 'sum=[0-9A-F]+' | tail -1)
  [ "$got" = "sum=$3" ] || { echo "$1: expected $3, got $got" >&2; exit 1; }
  echo "ok $1"
}
while read -r replay secs sum disc hd; do
  [ "${disc:-}" = - ] && disc=
  [ -z "$replay" ] || check "$replay" "$secs" "$sum" "${disc:-}" "${hd:-}"
done < tests/expected.txt
DREAM_WAV=build/sound.wav DREAM_REPLAY=tests/sound.replay ./build/archicast-host build/rom.bin assets/cmos.bin 97 /dev/null 2>/dev/null
python3 tools/soundcheck.py build/sound.wav
git push origin HEAD:main
git fetch -q origin
git status -sb | head -1
