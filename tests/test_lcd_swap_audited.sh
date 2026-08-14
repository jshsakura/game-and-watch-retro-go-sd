#!/usr/bin/env bash
# Nobody may add or move an lcd_swap() without deciding what it means for the
# drawn-frame counter.
#
# g_common_drawn_frames is incremented in lcd_swap(). That makes the DEFAULT
# honest -- ~68 call sites funnel through one function, so a new core is counted
# without knowing the counter exists -- and it makes exactly one mistake
# possible: skip the render, flip anyway, and every frame reports as drawn.
# Three cores do flip anyway, on purpose (holding the flip leaves the panel on a
# front buffer that goes stale), and they call lcd_swap_stale() instead. The
# counter this replaced had the mirror-image defect and put Virtual Boy in the
# ledger at 9 drawn fps while it was presenting 36.
#
# Both mistakes produce a number that looks like a measurement. Neither produces
# a compile error, a crash, or a visible glitch. So the check is a census:
# tests/lcd_swap_audited.txt lists every file that calls either function, with
# its counts and the verdict from reading its loop. When the tree and the list
# disagree, someone changed a flip and this says so.
#
# Deliberately NOT a heuristic. The obvious gate -- "flag any file that gates
# its render with if (drawFrame) and also calls lcd_swap()" -- flags a7800,
# amstrad, ngp, pkmini, wsv, wswan and nes_fceu, all of which are correct:
# their swap is INSIDE the guard. Telling those apart needs a parser, and a
# gate that cries wolf on seven correct cores is a gate people learn to skip.
# Counting is exact.
#
# Deliberately NO external tools. Five gates in this tree were SKIPPING in every
# CI release because nm and objdump were not on the container's PATH, including
# the cross-overlay alias check that CLAUDE.md calls the only thing standing
# between a core and another core's symbols. Nobody noticed, because locally
# they passed. This runs on grep and sed.
#
# What it cannot see: gw_firmware_abi.c hands lcd_swap out as a function
# pointer, and an external core calling through it is not in this tree. That
# gap is written down at the bottom of the audited list.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

LIST="tests/lcd_swap_audited.txt"
ROOTS="Core/Src retro-go-stm32"

echo "=== every lcd_swap() call site has been audited for the drawn counter ==="

if [ ! -f "$LIST" ]; then
    echo "  FAIL $LIST is missing -- the census has no baseline to compare against"
    echo
    echo "FAILED"
    exit 1
fi

# A CALL, not the name in prose. `^[^*/]*` is this tree's idiom for it (see
# test_ingame_overlay_wired.sh): a line whose first * or / precedes the match is
# a comment. Without it the paragraph above would count itself, and gw_lcd.c's
# own explanatory comment did exactly that in the first draft.
CALL_RE='^[^*/]*lcd_swap\('
STALE_RE='^[^*/]*lcd_swap_stale\('

count() { grep -coE "$2" "$1" 2>/dev/null || true; }

rc=0
checked=0

# --- the tree, as it is now -------------------------------------------------
actual="$(
    for f in $(grep -rlE "$CALL_RE|$STALE_RE" $ROOTS \
                    --include='*.c' --include='*.cpp' --include='*.cxx' 2>/dev/null | sort); do
        echo "$f swap=$(count "$f" "$CALL_RE") stale=$(count "$f" "$STALE_RE")"
    done
)"

# --- the list, as it was audited --------------------------------------------
expected="$(sed -e 's/#.*//' -e 's/[[:space:]]\+$//' "$LIST" \
            | grep -vE '^[[:space:]]*$' \
            | awk '{print $1, $2, $3}' | sort)"

# --- compare ----------------------------------------------------------------
while read -r path swap stale; do
    [ -n "${path:-}" ] || continue
    checked=$((checked + 1))
    want="$(printf '%s\n' "$expected" | awk -v p="$path" '$1==p {print $2, $3; exit}')"
    if [ -z "$want" ]; then
        echo "  FAIL $path calls lcd_swap() and is not in $LIST"
        echo "       Read its loop: when it SKIPS a frame, does new content still"
        echo "       reach the back buffer? Always -> lcd_swap() is right and"
        echo "       drawn == emu is the truth. Never -> lcd_swap_stale() on the"
        echo "       skip path. Sometimes -> the condition is the core's own."
        echo "       Then add the line, with which of those it is."
        rc=1
    elif [ "$want" != "$swap $stale" ]; then
        echo "  FAIL $path is audited as [$want] but the tree has [$swap $stale]"
        echo "       A flip was added, removed, or changed between counted and"
        echo "       uncounted. Re-read the loop and update $LIST."
        rc=1
    fi
done <<< "$actual"

# The other direction: a line for a file that no longer calls either function is
# a stale audit, and a stale audit is how a real core hides behind a green run.
while read -r path swap stale; do
    [ -n "${path:-}" ] || continue
    if ! printf '%s\n' "$actual" | awk -v p="$path" '$1==p {found=1} END {exit !found}'; then
        echo "  FAIL $LIST lists $path ($swap $stale) but nothing there calls lcd_swap()"
        echo "       The file moved or the call went away -- drop the line."
        rc=1
    fi
done <<< "$expected"

# The scope IS the check. A green run over three files proves nothing, and a
# broken grep fails open, so say so loudly rather than quietly.
if [ "$checked" -lt 30 ]; then
    echo "  FAIL only $checked call site(s) matched -- the scan is broken, not the tree"
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "  OK   all $checked file(s) that flip the panel are accounted for"
    echo
    echo "PASSED"
else
    echo
    echo "FAILED"
fi
exit "$rc"
