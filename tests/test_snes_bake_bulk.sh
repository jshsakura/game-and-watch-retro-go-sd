#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=build/snes-bulk-evidence/host
mkdir -p "$out"
core=external/sm/src/snes
git -C external/sm show 5b77015292343a11591e7244c2a926b24c41b090:src/snes/spin_bake.c > "$out/reference.c"
cc=${CC:-gcc}
flags=(-O2 -g -Wall -Wextra -ffunction-sections -fdata-sections -I"$core")
# The pre-experiment source is compiled under its own namespace. Its span
# implementation is independent of the candidate's bulk-off preprocessor arm.
"$cc" "${flags[@]}" -c "$out/reference.c" -o "$out/reference.o" \
  -Dg_bake=ref_g_bake -Dspin_bake_run_span=ref_spin_bake_run_span \
  -Dspin_bake_scan=ref_spin_bake_scan -Dspin_bake_reset=ref_spin_bake_reset \
  -Dspin_bake_frame_tick=ref_spin_bake_frame_tick
"$cc" "${flags[@]}" -DSNES_BAKE_BULK=1 -c "$core/spin_bake.c" -o "$out/candidate.o"
"$cc" "${flags[@]}" tests/test_snes_bake_bulk.c "$out/reference.o" \
  "$out/candidate.o" -Wl,--gc-sections -o "$out/differential"
"$out/differential"
# Memory/undefined-behaviour coverage of the same meaningful CPU boundary gate.
"$cc" "${flags[@]}" -fsanitize=address,undefined -DSNES_BAKE_BULK=1 \
  -c "$core/spin_bake.c" -o "$out/candidate-sanitized.o"
"$cc" "${flags[@]}" -fsanitize=address,undefined tests/test_snes_bake_bulk.c \
  "$out/reference.o" "$out/candidate-sanitized.o" -Wl,--gc-sections \
  -o "$out/differential-sanitized"
"$out/differential-sanitized"
