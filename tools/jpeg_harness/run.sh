#!/usr/bin/env bash
# Host tests for hw_jpeg_decoder.c. Three real bugs shipped here in three
# consecutive releases, each found only after the device died, each a pure
# state/arithmetic bug that never needed the peripheral to reproduce. Every
# test here compiles and links the REAL Core/Src/porting/lib/hw_jpeg_decoder.c
# (not a from-scratch reimplementation of its state machine) against a
# faithful fake of the ST HAL it's written to -- tools/jpeg_harness/hal_fake/,
# transcribed from Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_jpeg.c with
# file:line citations at every behaviour these bugs turned on. gcov (see
# tests/coverage.sh) measures real lines of the shipped file, not a stand-in.
#
#   lock_test.c     hjpeg->Lock/State stuck after a rejected frame (7ae5c0e8)
#   callback_test.c end-of-input callback misread as "image is bad" (59f8bde0)
#   floor_test.c    HAL floors InDataLength to x4 and drops the EOI (b8c05a49)
#
# GREEN links the CURRENT hw_jpeg_decoder.c -- the file this repo ships today.
# RED links the REAL historical file from immediately before its fix landed
# (git show <fix>^:path), not a #ifdef inside a reimplementation -- so a RED
# failure is the actual regression the device hit, not a stand-in for it. A
# test that cannot fail proves nothing, so this script builds and runs both
# and treats a clean RED build as a harness bug, not a good sign.
#
# coverage_test.c is separate: it is not pinned to any one shipped bug, so it
# has no historical commit to be RED against. It only reaches real entry
# points/branches the three bug-shaped tests don't happen to (JPEG_DecodeGetSize,
# the SrcSize==0 guard, 4:2:0/4:2:2 chroma, oversized-image rejection, a
# malformed-header GetInfo failure, JPEG_DecodeDeInit) — GREEN-only.
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-gcc}"
FLAGS="-O0 -g -Wall -Wextra -std=gnu11 -no-pie -Itools/jpeg_harness/hal_fake -ICore/Src/porting/lib"
OUT=/tmp/jpeg_harness
mkdir -p "$OUT"
DECODER=Core/Src/porting/lib/hw_jpeg_decoder.c
HAL_FAKE=tools/jpeg_harness/hal_fake/hal_jpeg_fake.c

rc=0
red_skipped=0

test_names="lock_test callback_test floor_test"
pre_fix_rev_of() {
    case "$1" in
        lock_test)     echo "7ae5c0e8^" ;;
        callback_test) echo "59f8bde0^" ;;
        floor_test)    echo "b8c05a49^" ;;
    esac
}

for t in $test_names; do
    src="tools/jpeg_harness/$t.c"
    rev="$(pre_fix_rev_of "$t")"
    red_decoder="$OUT/${t}_pre_fix_hw_jpeg_decoder.c"

    echo "=== $t : RED ($rev's hw_jpeg_decoder.c -- reproduces the shipped bug) ==="
    if ! git show "$rev:$DECODER" > "$red_decoder" 2>"$OUT/${t}_git_show.log"; then
        # CI clones shallow, so the pre-fix revision simply is not there. The RED run
        # is how we know the test can fail at all — it is not a check on the code, and
        # a missing one is a gap in the evidence, not a defect. Skipping is honest;
        # failing the build over it teaches people to ignore the build.
        echo "SKIP  $t: could not read $rev:$DECODER (shallow clone?) — see $OUT/${t}_git_show.log"
        red_skipped=$((red_skipped + 1))
        continue
    fi
    $CC $FLAGS -DPRE_FIX_BUILD -o "$OUT/${t}_red" "$src" "$red_decoder" "$HAL_FAKE"
    if "$OUT/${t}_red"; then
        echo "FAIL $t: pre-fix build passed — it does not reproduce the bug"
        rc=1
    else
        echo "PASS $t: pre-fix build fails, as the shipped bug did"
    fi

    echo "=== $t : GREEN (current hw_jpeg_decoder.c) ==="
    $CC $FLAGS -o "$OUT/$t" "$src" "$DECODER" "$HAL_FAKE"
    if "$OUT/$t"; then
        echo "PASS $t: current code passes"
    else
        echo "FAIL $t: current code does not pass"
        rc=1
    fi
    echo
done

echo "=== coverage_test : GREEN-only (current hw_jpeg_decoder.c, not a bug pin) ==="
$CC $FLAGS -o "$OUT/coverage_test" tools/jpeg_harness/coverage_test.c "$DECODER" "$HAL_FAKE"
if "$OUT/coverage_test"; then
    echo "PASS coverage_test: current code passes"
else
    echo "FAIL coverage_test: current code does not pass"
    rc=1
fi

if [ "$red_skipped" -gt 0 ]; then
    echo "  note: $red_skipped RED run(s) skipped — no history for the pre-fix revisions here."
    echo "        Run this on a full clone before trusting the GREENs."
fi
exit $rc
