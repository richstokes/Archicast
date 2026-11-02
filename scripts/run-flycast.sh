#!/bin/bash
# Launch the CDI in Flycast with serial logging. Set FLYCAST_LOG to capture it.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
image=${1:-$root/dist/Archicast.cdi}
flycast=${FLYCAST_BIN:-$HOME/.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast}
[ -x "$flycast" ] || flycast=/Applications/Flycast.app/Contents/MacOS/Flycast
options="config:Debug.SerialConsoleEnabled=yes,input:device1=0,input:device2=5,input:device3=6,input:maple_sdl_keyboard=1,input:maple_sdl_mouse=2"
mkdir -p "$root/build"
echo $$ > "$root/build/flycast.pid"  # exec keeps this PID; stop-flycast.sh signals only it
exec "$flycast" -config "$options" "$image"
