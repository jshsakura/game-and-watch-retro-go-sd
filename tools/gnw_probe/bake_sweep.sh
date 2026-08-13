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
  # -n on every ssh here: without it ssh swallows the index list the loop below
  # is reading from stdin, and the sweep silently measures one cartridge.
  ssh -n "$HOST" "python3 -m gnwmanager sdpush --file /tmp/snes_bench_index.txt --dest-path '/' \
      -- start 0x08100000" >/dev/null 2>&1
}

echo "index,arm,emu_fps,drawn_fps,ratio,scene"
for arm in ${ARMS_TO_RUN:-benchsave bench0 bench1}; do
  bash tools/gnw_probe/arm.sh flash "$arm" >/dev/null 2>&1
  while read -r idx; do
    [ -n "$idx" ] || continue
    set_index "$idx"
    if [ "$arm" = "benchsave" ]; then
      sleep 45          # boot + 1200 frames + the one-shot save
      echo "$idx,save,,,,"
      continue
    fi
    out=$(bash tools/gnw_probe/drawn_ab.sh "$arm" "$FRAMES" < /dev/null 2>&1 | tail -1)
    # Parse the parenthesised RATES. "emu=1886 drawn=..." also has digits before
    # the word drawn, and the first version of this reported a frame count as fps.
    emu=$(printf '%s' "$out"   | sed -n 's/.*(\([0-9.]*\) emulated fps.*/\1/p')
    drawn=$(printf '%s' "$out" | sed -n 's/.*, \([0-9.]*\) drawn fps.*/\1/p')
    ratio=$(printf '%s' "$out" | sed -n 's/.*ratio=\([0-9.]*\).*/\1/p')
    scene=$(printf '%s' "$out" | sed -n 's/.*scene=\([A-Za-z]*\).*/\1/p')
    echo "$idx,$arm,${emu:-},${drawn:-},${ratio:-},${scene:-}"
  done < "$LIST"
done
