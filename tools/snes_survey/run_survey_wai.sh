#!/bin/bash
# Build the SNES WAI dot-share harness (snes_survey_wai.c) and run it over a
# directory of ROMs. Usage: run_survey_wai.sh <rom-dir> [frames] [out.tsv]
#   ROM extensions scanned: .smc .sfc .swc .fig
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"      # worktree root
cd "$ROOT"

ROMDIR="${1:?usage: run_survey_wai.sh <rom-dir> [frames] [out.tsv]}"
FRAMES="${2:-600}"
OUT="${3:-/tmp/snes_survey_wai.tsv}"

O=/tmp/snes_survey_wai_build; mkdir -p "$O"; rm -f "$O"/*.o
CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim -I$HERE"
for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  gcc -c $CF "$f" -o "$O/$(basename ${f%.c}).o"
done
gcc -c $CF "$HERE/snes_survey_wai.c" -o "$O/survey_wai.o"
gcc -o "$O/survey_wai" "$O"/*.o -lm

: > "$OUT"
count=0
mapfile -d '' ROMS < <(find "$ROMDIR" -type f \
  \( -iname '*.smc' -o -iname '*.sfc' -o -iname '*.swc' -o -iname '*.fig' \) -print0)
total=${#ROMS[@]}
echo "found $total ROMs under $ROMDIR" >&2
for r in "${ROMS[@]}"; do
  line="$("$O/survey_wai" "$r" "$FRAMES" 2>/dev/null \
          | grep -P '\t(OK|LOAD_FAIL|NO_APU|OPEN_FAIL|READ_FAIL)\t' | head -1)"
  if [ -n "$line" ]; then
    echo "$line" >> "$OUT"
  else
    echo "$(basename "$r")	BOOT_CRASH	-	-	-" >> "$OUT"
  fi
  count=$((count+1))
  if [ $((count % 25)) -eq 0 ]; then printf "\r  surveyed %d / %d ..." "$count" "$total" >&2; fi
done
printf "\r  surveyed %d / %d\n" "$count" "$total" >&2
echo "" >&2
echo "wrote $OUT ($count ROMs)" >&2
