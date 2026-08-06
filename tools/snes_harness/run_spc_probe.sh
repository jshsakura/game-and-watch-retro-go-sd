#!/bin/bash
# What is the SPC700 actually doing, and can we stop doing it?
#
# Builds the general SNES core exactly as run.sh does, then links spc_idle_probe.c
# over spc_runOpcode with --wrap. Nothing in external/sm is touched.
#
#   SPC_SKIP=0   measure only: per-PC opcode/cycle histogram, hottest sites, and
#                the ARAM bytes around the hottest one (disassemble it by hand).
#   SPC_SKIP=1   the lever: charge the N-SPC timer-0 wait loop's idle iterations
#                in one step instead of dispatching them.
#   SPC_SKIP=2   RED arm: overshoot the tick by one 8-cycle iteration.
#   SPC_SKIP=3   RED arm: overshoot by a whole 128-cycle timer tick.
#
# The gate is the harness's own state= and audio= hashes: SKIP=1 must reproduce
# SKIP=0 bit for bit. It does, on ALTTP and SMW, 700 and 2500 frames. SKIP=3
# changes audio=, which is what proves the gate can fail at all. SKIP=2 does NOT
# — an 8-cycle slip is below what these hashes resolve, so do not read a passing
# hash as proof of cycle-exactness finer than ~a timer tick.
#
# Usage: run_spc_probe.sh <rom> [frames]
set -e
cd "$(dirname "$0")/../.."

ROM=${1:?usage: run_spc_probe.sh <rom> [frames]}
FRAMES=${2:-700}
OUT=${OUT:-/tmp/snes_spc_probe}
mkdir -p "$OUT"

CFLAGS="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w
        -Iexternal/sm -Itools/sm_harness/shim"

if [ ! -f "$OUT/.built" ]; then
  rm -f "$OUT"/*.o
  for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
    gcc -c $CFLAGS "$f" -o "$OUT/$(basename "${f%.c}").o"
  done
  gcc -c $CFLAGS tools/snes_harness/snes_main.c       -o "$OUT/main.o"
  gcc -c $CFLAGS tools/snes_harness/spc_idle_probe.c  -o "$OUT/probe.o"
  gcc -o "$OUT/snes" "$OUT"/*.o -lm -Wl,--wrap=spc_runOpcode
  touch "$OUT/.built"
fi

"$OUT/snes" "$ROM" "$FRAMES"
