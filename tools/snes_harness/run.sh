#!/bin/bash
# Build the general SNES core two ways and prove they are the same machine.
#
#   dot-loop : the reference. 178,684 calls a frame, ~178,000 of which only
#              increment a counter.
#   event    : the same events at the same dots, with the idle dots between them
#              charged in one subtraction instead of one loop iteration each.
#
# If the two do not agree on the state hash (framebuffer + WRAM + SRAM), the event
# loop is wrong and the number it prints is worthless. Check before you celebrate.
#
# Usage: run.sh <rom> [frames]
set -e
cd "$(dirname "$0")/../.."

ROM=${1:?usage: run.sh <rom> [frames]}
FRAMES=${2:-900}
OUT=${OUT:-/tmp/snes_harness}
mkdir -p "$OUT"

# The device's flags for this code: the emulation core is the hot path, -O2.
CFLAGS="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w
        -Iexternal/sm -Itools/sm_harness/shim"

build() {   # $1 = extra defines, $2 = tag
  local o="$OUT/$2"
  mkdir -p "$o" && rm -f "$o"/*.o
  for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
    gcc -c $CFLAGS $1 "$f" -o "$o/$(basename "${f%.c}").o"
  done
  gcc -c $CFLAGS $1 tools/snes_harness/snes_main.c -o "$o/main.o"
  gcc -o "$o/snes" "$o"/*.o -lm
}

build "-DSNES_DOT_LOOP" dot
build ""                event

a=$("$OUT/dot/snes"   "$ROM" "$FRAMES")
b=$("$OUT/event/snes" "$ROM" "$FRAMES")
echo "  $a"
echo "  $b"

ha=$(sed -n 's/.*state=\([0-9a-f]*\).*/\1/p' <<<"$a")
hb=$(sed -n 's/.*state=\([0-9a-f]*\).*/\1/p' <<<"$b")
if [ "$ha" != "$hb" ]; then
  echo "  FAIL: the event loop is not the same machine (state hashes differ)"
  exit 1
fi

ma=$(awk '{print $2}' <<<"$a")
mb=$(awk '{print $2}' <<<"$b")
awk -v a="$ma" -v b="$mb" 'BEGIN { printf "  ok: identical state, %.2fx faster\n", a/b }'
