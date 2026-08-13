#!/bin/bash
# Survey a ROM library with the firmware's own loader and recognizer.
#   run.sh <rom-dir> [out.tsv]
set -euo pipefail
cd "$(dirname "$0")/../.."
DIR="${1:?usage: run.sh <rom-dir> [out.tsv]}"
OUT="${2:-/tmp/snes_bake_survey.tsv}"
O=/tmp/snes_bake_survey_build; mkdir -p "$O"
SM=external/sm/src
CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DSNES_SPIN_BAKE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"
gcc $CF $SM/snes/*.c $SM/tracing.c tools/snes_bake_survey/survey.c -o "$O/survey" -lm
find "$DIR" -maxdepth 2 -type f \( -iname '*.smc' -o -iname '*.sfc' \) -print0 \
  | xargs -0 "$O/survey" > "$OUT"
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) ROMs)" >&2
