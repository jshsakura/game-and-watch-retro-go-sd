#!/bin/bash
# Run the WAI dot-share survey (snes_survey_wai.c) over the whole SNES ROM
# library entirely on the RPi5, and bring back only the summary. No ROM, no
# per-ROM row, and no build log ever touches the caller's context.
#
# Mirrors tools/snes_survey/remote_survey.sh's shape.
#
# Usage: bash tools/snes_survey/remote_wai.sh [rom-dir-on-pi] [frames]
set -euo pipefail

REMOTE=rpi-genie5
ROMDIR="${1:-/home/pi/app/ganda-rpi/data/library/public/roms/snes}"
FRAMES="${2:-600}"
RWORK=/tmp/snes_wai_remote

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

echo "[1/4] rsyncing the WAI survey tree to $REMOTE:$RWORK" >&2
ssh "$REMOTE" "mkdir -p $RWORK"
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
CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim -I$HERE"
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  gcc -c $CF "$f" -o "$O/$(basename ${f%.c}).o"
done
gcc -c $CF "$HERE/snes_survey_wai.c" -o "$O/survey_wai.o"
gcc -o "$O/survey_wai" "$O"/*.o -lm
echo "build OK" >&2

: > "$RWORK/wai.tsv"
mapfile -d '' ROMS < <(find "$ROMDIR" -type f \
  \( -iname '*.smc' -o -iname '*.sfc' -o -iname '*.swc' -o -iname '*.fig' \) -print0)
echo "found ${#ROMS[@]} ROMs" >&2
n=0
for r in "${ROMS[@]}"; do
  line=$(timeout 30 "$O/survey_wai" "$r" "$FRAMES" 2>/dev/null | tail -1) || line=$'\t'crash
  printf '%s\n' "$line" >> "$RWORK/wai.tsv"
  n=$((n+1)); (( n % 250 == 0 )) && echo "  ...$n/${#ROMS[@]}" >&2
done
echo "swept $n ROMs" >&2
REMOTE_EOF

echo "[3/4] building histogram on $REMOTE (summary only)" >&2
scp -q "$HERE/wai_classify.py" "$REMOTE:$RWORK/tools/snes_survey/wai_classify.py"
ssh "$REMOTE" "cd $RWORK && python3 tools/snes_survey/wai_classify.py wai.tsv" > "$HERE/wai_summary.txt"

echo "[4/4] done -> tools/snes_survey/wai_summary.txt" >&2
echo "======================================================================"
cat "$HERE/wai_summary.txt"
