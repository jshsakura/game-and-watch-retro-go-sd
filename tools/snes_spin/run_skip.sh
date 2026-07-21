#!/bin/bash
# Build the exact-replay spin-skip harness and gate it: SNES_SKIP=0 vs 1 must
# print identical state/audio hashes. Usage: run_skip.sh <rom> [frames]
#
# The learner and the cpu.c purity hooks are IN the core now
# (src/snes/spin_skip.c, cpu.c under -DSNES_SPIN_SKIP) — no sed-instrumented
# copies: this compiles the very files the device firmware compiles.
set -e
cd "$(dirname "$0")/../.."
O=/tmp/snes_skip; mkdir -p "$O"; rm -f "$O"/*.o
SM=external/sm/src

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DSNES_SPIN_SKIP -DHEADLESS -w -Iexternal/sm -Iexternal/sm/src/snes -Itools/sm_harness/shim"
for f in $SM/snes/*.c; do
  gcc -c $CF "$f" -o "$O/$(basename "${f%.c}").o"
done
gcc -c $CF "$SM/tracing.c" -o "$O/tracing.o"
gcc -c $CF tools/snes_spin/skip_harness.c -o "$O/skip.o"
gcc -o "$O/skip" "$O"/*.o -lm

echo "--- baseline (skip off) ---"
SNES_SKIP=0 "$O/skip" "$1" "${2:-1500}"
echo "--- skip on ---"
SNES_SKIP=1 "$O/skip" "$1" "${2:-1500}"
