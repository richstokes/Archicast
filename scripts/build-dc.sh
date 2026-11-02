#!/bin/bash
# Regenerate the recompiled ROM and build the Dreamcast ELF and CDI.
# DC_MAKE_ARGS adds make variables, DC_CDI names the image.
#   --skip-gen                        reuse the recompiled ROM
#   --include-archive-software on|off bundle the Archimedes software archive as a
#                                     hard disc on the CD (default on; downloads are
#                                     cached under /tmp/archimedes-archive)
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
skip_gen= archive=on
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-gen) skip_gen=1 ;;
    --include-archive-software=*) archive=${1#*=} ;;
    --include-archive-software) shift; archive=${1:-} ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
case "$archive" in on|off) ;; *) echo "--include-archive-software takes on or off" >&2; exit 2 ;; esac
# Bundled disc-image tool (tools/mkdcdisc). MKDCDISC overrides it with another binary.
# Built with the host toolchain, so this happens before the KOS environment is sourced.
if [ -z "${MKDCDISC:-}" ]; then make mkdcdisc; MKDCDISC=build/mkdcdisc/mkdcdisc; fi
# The archive image only depends on the tools and the cache, not on the ROM
[ "$archive" = off ] || make build/archive.hd
set +u; source "${KOS_ENV:-$HOME/.local/share/dreamcast/kos/environ.sh}"; set -u
[ -n "$skip_gen" ] || make gen
make -j"$(getconf _NPROCESSORS_ONLN)" dc ${DC_MAKE_ARGS:-}
# Overlays run at a fixed address inside the ELF, so they link against it last.
region=0x$(sh-elf-nm build/archicast.elf | awk '$3 == "_overlay_region" {print $1}')
bytes=$(grep -o 'OVERLAY_BYTES 0x[0-9a-f]*u' src/cpu.h | sed 's/.* //; s/u$//')
rm -rf build/disc; mkdir -p build/disc
shopt -s nullglob # app units are a single file
cp build/rom.bin build/disc/rom.bin
if [ -n "${DC_ADF:-}" ]; then cp "$DC_ADF" build/disc/adfs.adf; else gunzip -c assets/adfs.adf.gz > build/disc/adfs.adf; fi
[ "$archive" = off ] || cp build/archive.hd build/disc/archive.hd
for table in build/dc/ov_*_table.o; do
  unit=$(basename "$table" | cut -d_ -f2)
  sh-elf-ld -T tools/overlay.ld --defsym=REGION="$region" --just-symbols=build/archicast.elf \
    "$table" build/dc/ov_"$unit"_[0-9]*.o -o build/ov_"$unit".elf
  sh-elf-objcopy -O binary build/ov_"$unit".elf build/disc/ov_"$unit".bin
  # Whole sectors: a read of ov_4's short last sector never returned in Flycast
  truncate -s %2048 build/disc/ov_"$unit".bin
  size=$(wc -c < build/disc/ov_"$unit".bin)
  echo "overlay $unit: $size bytes"
  [ "$size" -le $((bytes)) ] || { echo "overlay $unit does not fit in $bytes" >&2; exit 1; }
done

mkdir -p dist
"$MKDCDISC" --allow-overwrite --disable-data-track-padding --main-elf build/archicast.elf --directory-contents build/disc \
  --title 'Archicast' --author 'richstokes' --bootstrap-mr-image assets/acorn-logo.png --output "${DC_CDI:-dist/Archicast.cdi}"
sh-elf-size build/archicast.elf
