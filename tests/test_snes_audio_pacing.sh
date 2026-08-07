#!/usr/bin/env bash
# RED-first integration test for the SNES audio pacing block.
#
# It extracts the ACTUAL pacing block from Core/Src/porting/snes/main_snes.c
# (the if-speedupEnabled brace-delimited section), compiles it against the
# real stretcher and a mock DMA/audio layer, and drives it at a range of fps.
#
#   RED:   extract from BUGGY_REV (pinned), the commit that had the catch-up emit loop
#   GREEN: extract from the working tree — the (void)elapsed; fix
#
# RED MUST fail (invariants violated), GREEN MUST pass. A test that has never
# been red proves nothing — see CLAUDE.md "RED before GREEN, and RED against
# the real thing".
#
# The block is extracted by brace-matching, not line numbers, so it survives
# edits. If the markers move, the awk says so and the test fails loudly rather
# than silently extracting the wrong code.
set -e
cd "$(dirname "$0")/.."

# The RED arm must name the commit that HAD the bug. Reading it from HEAD works
# exactly once -- the moment the fix is committed, HEAD is the fixed code, RED
# stops failing, and this test fails the build for everyone with a green tree.
# That is the third time a safety net in this repo has been the thing that broke
# CI, so: pin the revision, and if the clone cannot see it (shallow), SKIP and
# say so rather than fail.
BUGGY_REV=${BUGGY_REV:-daae17d0}
if ! git cat-file -e "$BUGGY_REV:Core/Src/porting/snes/main_snes.c" 2>/dev/null; then
  echo "SKIP no $BUGGY_REV in this clone (shallow?) - RED check not run"
  echo "     GREEN invariants still checked below."
  SKIP_RED=1
fi

CC="${CC:-gcc}"
CFLAGS="-O2 -Wall -Wextra -std=c11 -g"
SRC="Core/Src/porting/snes"
STRETCHER="$SRC/snes_audio_stretch.c"
TEST_C="tests/test_snes_audio_pacing.c"
MAIN="Core/Src/porting/snes/main_snes.c"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- extractor: pull the pacing block from a file on stdin -----------------
# Finds the `if (odroid_system_get_app()->speedupEnabled == SPEEDUP_1x) {` line,
# then the first `    }` (4-space indent) after it — that is the closing brace
# of the outer if-block. Prints everything between, inclusive.
extract_pacing() {
    awk '
        /if \(odroid_system_get_app\(\)->speedupEnabled == SPEEDUP_1x\)/ { started = 1 }
        started { print }
        started && /^    \}[[:space:]]*$/ { exit }
    '
}

# --- compile + run one variant ---------------------------------------------
# $1 = label (RED or GREEN), $2 = source of main_snes.c ("-" for stdin/git show)
# Returns 0 if the expectation was met, 1 if not.
run_variant() {
    local label="$1" src_cmd="$2" expect_fail="$3"
    local inc="$TMP/snes_pacing_block.inc"

    if [ "$src_cmd" = "WORKTREE" ]; then
        extract_pacing < "$MAIN" > "$inc"
    else
        git show "$BUGGY_REV:$MAIN" | extract_pacing > "$inc"
    fi

    if [ ! -s "$inc" ]; then
        echo "  [$label] FAIL: extracted block is empty"
        return 1
    fi

    local bin="$TMP/test_${label}"
    if ! $CC $CFLAGS -I"$TMP" "$TEST_C" "$STRETCHER" -o "$bin" 2>"$TMP/err_${label}"; then
        echo "  [$label] COMPILE ERROR:"
        cat "$TMP/err_${label}"
        return 1
    fi

    # Run and capture output + exit code
    set +e
    "$bin" > "$TMP/out_${label}" 2>&1
    local rc=$?
    set -e

    # Show the tail of the output
    tail -5 "$TMP/out_${label}" | sed 's/^/    /'

    if [ "$expect_fail" = "yes" ]; then
        # RED: test binary SHOULD fail (invariants violated by buggy block)
        if [ $rc -ne 0 ]; then
            echo "  [$label] PASS: buggy block correctly failed invariants (rc=$rc)"
            return 0
        else
            echo "  [$label] FAIL: buggy block PASSED — test does not catch the bug!"
            return 1
        fi
    else
        # GREEN: test binary SHOULD pass (invariants hold with fixed block)
        if [ $rc -eq 0 ]; then
            echo "  [$label] PASS: fixed block correctly passed all invariants"
            return 0
        else
            echo "  [$label] FAIL: fixed block failed invariants (rc=$rc)"
            return 1
        fi
    fi
}

echo "=== SNES audio pacing block — RED / GREEN ==="
echo "  DMA period = 16.625 ms (266 samples @ 16 kHz)"
echo "  Bug: catch-up loop emits 2-3x per DMA period at fps < ~32"
echo "  Fix: (void)elapsed; — no catch-up emit"
echo

rc=0

if [ -n "${SKIP_RED:-}" ]; then
    echo "--- RED: skipped (pinned revision not in this clone) ---"
else
    echo "--- RED: $BUGGY_REV (buggy catch-up emit loop) ---"
    run_variant "RED" "BUGGY" "yes" || rc=1
fi
echo

echo "--- GREEN: working tree (fixed) ---"
run_variant "GREEN" "WORKTREE" "no" || rc=1
echo

if [ $rc -eq 0 ]; then
    if [ -n "${SKIP_RED:-}" ]; then
        echo "=== RESULT: PASS (GREEN passed; RED skipped, see above) ==="
    else
        echo "=== RESULT: PASS (RED failed as expected, GREEN passed) ==="
    fi
else
    echo "=== RESULT: FAIL ==="
fi
exit $rc
