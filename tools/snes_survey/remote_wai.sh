#!/bin/bash
# Classify the whole SNES ROM library by sound driver, entirely on the RPi5,
# and bring back only the summary. No ROM, no per-ROM row, and no build log
# ever touches the caller's context -- the survey compiles and runs where the
# ROMs already live (rpi-genie5), and only classify.py's histogram is printed.
#
# Same idea as the GBA M4A survey: fingerprint the whole library, group by
# sound driver, then build one HLE per group by descending ROM count.
#
# Usage: bash tools/snes_survey/remote_survey.sh [rom-dir-on-pi] [frames]
set -euo pipefail

REMOTE=rpi-genie5
ROMDIR="${1:-/home/pi/app/ganda-rpi/data/library/public/roms/snes}"
FRAMES="${2:-180}"
RWORK=/tmp/snes_wai_remote

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

echo "[1/4] rsyncing the survey tree to $REMOTE:$RWORK" >&2
ssh "$REMOTE" "mkdir -p $RWORK"
# Ship exactly what run_survey.sh compiles against, with the paths it expects:
#   external/sm/**  tools/snes_survey/**  tools/sm_harness/shim/**
rsync -az --delete \
  --include='external/' --include='external/sm/***' \
  --include='tools/' --include='tools/snes_survey/***' \
  --include='tools/sm_harness/' --include='tools/sm_harness/shim/***' \
  --exclude='*' \
  "$REPO"/ "$REMOTE:$RWORK/" >&2

echo "[2/4] building + sweeping $ROMDIR ($FRAMES f/ROM) on $REMOTE -- native gcc" >&2
ssh "$REMOTE" bash -s "$RWORK" "$ROMDIR" "$FRAMES" <<'REMOTE_EOF'
set -euo pipefail
RWORK="$1"; ROMDIR="$2"; FRAMES="$3"
cd "$RWORK"
HERE="$RWORK/tools/snes_survey"
O=/tmp/snes_wai_build; mkdir -p "$O"; rm -f "$O"/*.o
# verbatim from run_survey.sh (defines + include paths that gate apu/cpu in)
CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim -I$HERE"
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  gcc -c $CF "$f" -o "$O/$(basename ${f%.c}).o"
done
gcc -c $CF "$HERE/snes_wai.c" -o "$O/wai.o"
gcc -o "$O/survey" "$O"/*.o -lm
echo "build OK" >&2

: > "$RWORK/wai.tsv"
mapfile -d '' ROMS < <(find "$ROMDIR" -type f \
  \( -iname '*.smc' -o -iname '*.sfc' -o -iname '*.swc' -o -iname '*.fig' \) -print0)
echo "found ${#ROMS[@]} ROMs" >&2
n=0
for r in "${ROMS[@]}"; do
  line=$(timeout 30 "$O/survey" "$r" "$FRAMES" 2>/dev/null | tail -1) || line=$'\t'crash
  printf '%s\n' "$line" >> "$RWORK/wai.tsv"
  n=$((n+1)); (( n % 250 == 0 )) && echo "  ...$n/${#ROMS[@]}" >&2
done
echo "swept $n ROMs" >&2
REMOTE_EOF

echo "[3/4] classifying on $REMOTE (summary only)" >&2
ssh "$REMOTE" "cd $RWORK && python3 tools/snes_wai.classify.py wai.tsv" > "$HERE/wai_summary.txt"

echo "[4/4] done -> tools/snes_survey/wai_summary.txt" >&2
echo "======================================================================"
cat "$HERE/wai_summary.txt"
