#!/usr/bin/env bash
# Device A/B over many cartridges, with three flashes instead of two per ROM.
#
#   bake_sweep.sh <index-list-file> [frames]
#
# The autoboot index used to be compile-time, so measuring N cartridges meant
# 2N container builds -- 55 hours for a 413-cartridge shortlist, which is why
# nobody had done it. rg_emulators.c now reads /snes_bench_index.txt off the
# card, so the ROM under test is a one-line file and a reset.
#
# Three passes, each preceded by exactly one flash:
#   1. benchsave  writes savestate slot 0 for every index (the console plays
#                 itself; nobody can sit at a console with a probe on it)
#   2. bench0     the shipping build, resumed from those states
#   3. bench1     SNES_SPIN_BAKE=1, same states
#
# Emits CSV on stdout: index,arm,emu_fps,drawn_fps,ratio
set -eu
cd "$(dirname "$0")/../.."
LIST=${1:?usage: bake_sweep.sh <index-list-file> [frames]}
FRAMES=${2:-1800}
HOST=${PROBE_HOST:-rpi-genie5}

set_index() {   # $1 = index
  printf '%s\n' "$1" > /tmp/snes_bench_index.txt
  scp -q /tmp/snes_bench_index.txt "$HOST:/tmp/" 
  ssh "$HOST" "python3 -m gnwmanager sdpush --file /tmp/snes_bench_index.txt --dest-path '/' \
      -- start 0x08100000" >/dev/null 2>&1
}

echo "index,arm,emu_fps,drawn_fps,ratio"
for arm in benchsave bench0 bench1; do
  bash tools/gnw_probe/arm.sh flash "$arm" >/dev/null 2>&1
  while read -r idx; do
    [ -n "$idx" ] || continue
    set_index "$idx"
    if [ "$arm" = "benchsave" ]; then
      sleep 45          # boot + 1200 frames + the one-shot save
      echo "$idx,save,,," 
      continue
    fi
    out=$(bash tools/gnw_probe/drawn_ab.sh "$arm" "$FRAMES" 2>&1 | tail -1)
    emu=$(printf '%s' "$out" | grep -oE '\(([0-9.]+) emulated' | grep -oE '[0-9.]+')
    drawn=$(printf '%s' "$out" | grep -oE '([0-9.]+) drawn' | grep -oE '[0-9.]+')
    ratio=$(printf '%s' "$out" | grep -oE 'ratio=[0-9.]+' | cut -d= -f2)
    scene=$(printf '%s' "$out" | grep -oE 'scene=[A-Za-z ]+' | cut -d= -f2)
    echo "$idx,$arm,${emu:-},${drawn:-},${ratio:-}  # $scene"
  done < "$LIST"
done
