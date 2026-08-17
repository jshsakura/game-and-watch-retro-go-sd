#!/usr/bin/env bash
# Cx4 HLE harness.
#
# What it proves:
#   1. The chip's RAM writes stay inside the chip's RAM. Three lengths in
#      cx4_hle.c come straight from the cartridge -- the $7f47 DMA length is a
#      full 16 bits, and the two image commands size their clear from a width
#      and a height byte -- while the buffer is 8 KB, allocated out of a heap
#      the launcher shares. Under ASan, an unclamped build dies here.
#   2. The wireframe chain-walk terminates. Its loop condition reads a pointer
#      the body never advances, so a segment list of $ffff markers spins for
#      ever; on the device that is a hang inside a cart write handler.
#   3. A fixed command sweep still hashes the same. This is a regression net,
#      not a correctness proof -- see PROVENANCE below for what is and is not
#      established about matching real hardware.
#
# It compiles external/sm/src/snes/cx4_hle.c itself. Not a copy, not a vendored
# snapshot: a harness that builds its own version of the file is a different
# program, which is how three device-killing bugs shipped out of a file that
# had three green tests.
#
# Usage:
#   run.sh              GREEN: current tree must pass every mode
#   run.sh --red        RED:   the pre-clamp file out of git history must fail
#   run.sh --bless      regenerate golden_sweep.txt (review the diff!)
#
# PROVENANCE. The bit-identical claim in cx4_hle.h was established against a
# behavioral reference that cannot live in this tree -- its licence is
# non-commercial and this repository is GPLv2. That comparison is reproducible
# but not automatic: point CX4_ORACLE at a reference build and diff its command
# stream by hand. What runs in CI is everything above, which needs no oracle.

set -u
cd "$(dirname "$0")/../.."

HERE=tools/cx4_harness
SM=external/sm/src/snes
OUT=${TMPDIR:-/tmp}/cx4_harness.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

CC=${CC:-gcc}
CF="-O1 -g -std=c11 -Wall -Wextra -Werror=implicit-function-declaration
    -fsanitize=address,undefined -fno-sanitize-recover=all -I$SM -I$HERE"

fail=0
note() { printf '%s\n' "$*"; }
bad()  { printf 'CX4 FAIL: %s\n' "$*"; fail=1; }
skip() { printf 'CX4 SKIPPED: %s\n' "$*"; }

command -v "$CC" >/dev/null 2>&1 || { skip "no $CC on PATH -- nothing built"; exit 0; }

# The file under test lives in a submodule. A job that has not checked it out
# must not fail the build over it -- but it must say so, loudly, every run: this
# tree has already shipped releases whose gates were all quietly SKIPPED.
# The fix for a skip you did not want is in the workflow, not here.
[ -f "$SM/cx4_hle.c" ] || {
  skip "$SM/cx4_hle.c absent (external/sm not checked out) -- the Cx4 gate did NOT run"
  exit 0
}

# ---- build ----------------------------------------------------------------
# $1 = path to the cx4_hle.c to test, $2 = output binary
build() {
  # shellcheck disable=SC2086
  $CC $CF "$1" "$HERE/cx4_driver.c" -o "$2" -lm 2>"$OUT/cc.log"
}

MODES_OVERFLOW="overflow-dma overflow-scale overflow-disint"

# ---- RED ------------------------------------------------------------------
if [ "${1:-}" = "--red" ]; then
  # The commit that introduced the clamp; its parent is the file that has to
  # fail. Searched for rather than pinned, so a rebase does not silently turn
  # this gate into a no-op.
  fix=$(git -C external/sm log --format=%H -1 -S clamp_to_ram -- src/snes/cx4_hle.c 2>/dev/null)
  if [ -z "$fix" ]; then
    skip "no pre-clamp revision reachable (shallow clone?) -- RED not run"
    exit 0
  fi
  if ! git -C external/sm show "$fix^:src/snes/cx4_hle.c" > "$OUT/pre.c" 2>/dev/null; then
    skip "cannot read $fix^:src/snes/cx4_hle.c -- RED not run"
    exit 0
  fi
  # It needs its own header next to it to compile as cx4_hle.c would.
  cp "$SM/cx4_hle.h" "$OUT/cx4_hle.h"
  if ! $CC $CF -I"$OUT" "$OUT/pre.c" "$HERE/cx4_driver.c" -o "$OUT/red" -lm 2>"$OUT/cc.log"; then
    bad "the pre-clamp file did not compile; RED proves nothing"
    sed -n '1,20p' "$OUT/cc.log"
    exit 1
  fi

  for m in $MODES_OVERFLOW; do
    if "$OUT/red" "$m" >/dev/null 2>"$OUT/$m.err"; then
      bad "RED $m: pre-clamp build PASSED -- the gate is not testing the bug"
    else
      note "RED $m: pre-clamp build died as expected ($(grep -om1 'heap-buffer-overflow\|SEGV\|runtime error' "$OUT/$m.err" || echo 'non-zero exit'))"
    fi
  done

  if timeout 10 "$OUT/red" hang-wireframe >/dev/null 2>&1; then
    bad "RED hang-wireframe: pre-fix build TERMINATED -- the gate is not testing the hang"
  else
    note "RED hang-wireframe: pre-fix build did not terminate within 10s, as expected"
  fi

  [ $fail -eq 0 ] && note "CX4 RED: OK (every gate reproduced its fault)"
  exit $fail
fi

# ---- GREEN ----------------------------------------------------------------
if ! build "$SM/cx4_hle.c" "$OUT/green"; then
  bad "current cx4_hle.c did not compile"
  sed -n '1,30p' "$OUT/cc.log"
  exit 1
fi

for m in $MODES_OVERFLOW; do
  if "$OUT/green" "$m" >/dev/null 2>"$OUT/$m.err"; then
    note "GREEN $m: in bounds"
  else
    bad "GREEN $m: $(grep -om1 'heap-buffer-overflow\|SEGV\|runtime error' "$OUT/$m.err" || echo 'non-zero exit') -- see below"
    sed -n '1,15p' "$OUT/$m.err"
  fi
done

if timeout 10 "$OUT/green" hang-wireframe >/dev/null 2>"$OUT/hang.err"; then
  note "GREEN hang-wireframe: terminated"
else
  bad "GREEN hang-wireframe: did not terminate within 10s (or died) -- the chain-walk is unbounded again"
  sed -n '1,15p' "$OUT/hang.err"
fi

# ---- regression sweep -----------------------------------------------------
GOLD="$HERE/golden_sweep.txt"
if ! "$OUT/green" sweep > "$OUT/sweep.txt" 2>"$OUT/sweep.err"; then
  bad "GREEN sweep: run failed"
  sed -n '1,15p' "$OUT/sweep.err"
elif [ "${1:-}" = "--bless" ]; then
  cp "$OUT/sweep.txt" "$GOLD"
  note "CX4 blessed: $GOLD rewritten ($(wc -l < "$GOLD") steps) -- review the diff before committing"
elif [ ! -f "$GOLD" ]; then
  skip "no $GOLD yet -- run with --bless to create it"
elif diff -u "$GOLD" "$OUT/sweep.txt" > "$OUT/sweep.diff"; then
  note "GREEN sweep: $(wc -l < "$GOLD") steps match golden"
else
  bad "GREEN sweep: command output moved -- the failing steps are:"
  grep '^[+-]mode=' "$OUT/sweep.diff" | head -20
fi

if [ $fail -eq 0 ]; then
  note "CX4: OK"
else
  note "CX4: FAILED"
fi
exit $fail
