#!/usr/bin/env bash
# Every core that takes the PAUSE shortcuts must also DRAW their feedback.
#
# common_emu_input_loop() acts on the shortcuts -- volume, brightness, speedup,
# save, load, turbo -- and then arms an overlay for the core to draw. Drawing it is
# a separate call, common_ingame_overlay(), made by each core between its own render
# and lcd_swap(). A core that calls the first and not the second changes the volume
# with nothing on screen to say so, which reads as "the shortcut overlay is broken".
#
# TamaPoke shipped exactly that. It is the same omission as its missing pause menu
# (common_emu_frame_loop paces, common_emu_input_loop menus -- having one is not
# having the other), as its dead Save/Load, and as its autosave hooks left NULL. The
# functions were fine every time; nothing called them. This is the check that sees a
# caller that never calls.
#
# Two things this deliberately does NOT do:
#
#   - It does not match the NAME, it matches a CALL (tests/c_strip.sh). The name
#     appears in prose, and matching prose made an earlier version accuse
#     tamapoke_input.cpp, whose only mention of it is a comment about what the real
#     caller does.
#   - It does not ask make which files are built. That version asked `make -pn` and
#     was FLAKY: three consecutive runs checked 27, 26 and 27 cores, with different
#     files sliding into "not built" each time, because a dry run re-evaluates the
#     Makefile's $(shell ...) calls. A gate whose scope silently varies is worse than
#     no gate -- a green run proves nothing about the core it quietly dropped. The
#     dead file is named here instead, with its reason: stable, and readable.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

# Not built by any configuration in this tree. Keep the reason with the name; an
# unexplained exception list is where a real core goes to hide.
is_dead() {
    case "$1" in
        # Makefile: "The old chips core (c64_impl.c/main_c64.c, .prg only) is no
        # longer built" -- the live C64 entry is main_c64_dev.cpp, which does draw it.
        */c64/main_c64.c) return 0 ;;
    esac
    return 1
}

echo "=== every core that takes the PAUSE shortcuts draws their overlay ==="

# The scanning used to be `grep -rlE '^[^*/]*common_emu_input_loop\('`, both to
# find the files and to test them. That pattern cannot match a line whose first
# * or / precedes the call, so `if (*p) common_emu_input_loop();` was invisible
# -- and because the same pattern did the DISCOVERY, such a file would not have
# been reported as unchecked; it would simply never have entered the loop. The
# gate would pass having looked at nothing. That is not theory: the same idiom
# in the lcd_swap census matched two of its six calls.
#
# Nothing in the tree trips it today (checked: the only file the old pattern
# excluded is tamapoke_input.cpp, correctly, for a comment). It is replaced
# anyway, because the failure is silent and the next core to be written is the
# one that pays.
. "$(dirname "$0")/c_strip.sh"

rc=0
checked=0
dead=""

# Discovery is loose (any mention), and c_calls decides. Fail-closed: a file
# that mentions the name and turns out to have no call is simply not counted,
# but it can never vanish before being looked at.
for f in $(grep -rl 'common_emu_input_loop' Core/Src/porting 2>/dev/null | sort); do
    case "$f" in
        *porting/common.c) continue ;;   # it IS the input loop
    esac
    [ "$(c_calls "$f" common_emu_input_loop)" -gt 0 ] || continue

    if is_dead "$f"; then
        dead="$dead $(basename "$f")"
        continue
    fi

    checked=$((checked + 1))
    if [ "$(c_calls "$f" common_ingame_overlay)" -eq 0 ]; then
        echo "  FAIL $f takes the shortcuts and never draws the overlay --"
        echo "       volume and brightness change with nothing on screen to say so"
        rc=1
    fi
done

if [ "$checked" -lt 20 ]; then
    # The scope is the point. If this ever collapses, say so loudly rather than
    # reporting a green run over three files.
    echo "  FAIL only $checked core(s) matched -- the scan is broken, not the tree"
    rc=1
fi

[ -n "$dead" ] && echo "       (not built, excluded by name:$dead)"

if [ "$rc" -eq 0 ]; then
    echo "  OK   all $checked core(s) that take the shortcuts draw the overlay"
    echo
    echo "PASSED"
else
    echo
    echo "FAILED"
fi
exit "$rc"
