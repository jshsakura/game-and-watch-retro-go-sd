#!/bin/bash
# Build + run the S-DSP block-mixer A/B (tools/dsp_mixer/).
#   run_ab.sh <rom> [frames] [bench-reps]
# apu.c is compiled with dsp_cycle/dsp_write renamed to hooks so the harness can
# record the per-frame cycle counts and register-write stream; everything else
# compiles read-only from external/ (never edited).
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_ab.sh <rom> [frames] [reps]}"
FRAMES="${2:-1200}"
REPS="${3:-3}"

O=/tmp/dsp_mixer_build; mkdir -p "$O"; rm -f "$O"/*.o
CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DDSP_MIXER_DIAG -w -Iexternal/sm -Itools/sm_harness/shim"

for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  b="$(basename "${f%.c}")"
  if [ "$b" = "apu" ]; then
    gcc -c $CF -Ddsp_cycle=hook_dsp_cycle -Ddsp_write=hook_dsp_write "$f" -o "$O/apu.o"
  else
    gcc -c $CF "$f" -o "$O/$b.o"
  fi
done
gcc -c $CF tools/dsp_mixer/mixer_block.c -o "$O/mixer_block.o"
gcc -c $CF tools/dsp_mixer/mixer_ab.c -o "$O/mixer_ab.o"
gcc -o "$O/mixer_ab" "$O"/*.o -lm

"$O/mixer_ab" "$ROM" "$FRAMES" "$REPS"
