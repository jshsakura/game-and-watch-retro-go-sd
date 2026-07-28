#!/usr/bin/env bash
# Every core that takes the PAUSE shortcuts must also DRAW their feedback.
#
# common_emu_input_loop() acts on the shortcuts -- volume, brightness, speedup,
# save, load -- and then arms an overlay for the core to draw. Drawing it is a
# separate call, common_ingame_overlay(), made by each core between its own render
# and lcd_swap(). A core that calls the first and not the second changes the volume
# with nothing on screen to say so, which reads as "the shortcut overlay is broken".
#
# TamaPoke shipped exactly that. It is the same omission as its missing pause menu
# (common_emu_frame_loop paces, common_emu_input_loop menus -- having one is not
# having the other) and as Super Metroid's dead Save/Load: the function was fine,
# nothing called it. This is the check that sees a caller that never calls.
#
# Only files the build actually compiles are checked, and the list comes from MAKE
# ITSELF (`make -pn`, its own variable database) rather than from grepping the
# Makefile. Grepping matched Core/Src/porting/c64/main_c64.c inside a COMMENT saying
# that file is no longer built -- so the first version of this gate opened with a
# failure about a dead pre-Frodo entry, and a gate whose first line is noise is a
# gate people learn to skim.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

echo "=== every core that takes the PAUSE shortcuts draws their overlay ==="

rc=0
checked=0
skipped=""

BUILT="$(timeout 180 make -pn CHECK_DIRTY_SUBMODULE=0 2>/dev/null \
         | grep -oE '[A-Za-z0-9_/.-]+\.(c|cpp)' | sort -u)"
if [ -z "$BUILT" ]; then
    echo "SKIP: could not ask make for its source list (no toolchain?)"
    exit 0
fi

for f in $(grep -rl "common_emu_input_loop" Core/Src/porting 2>/dev/null | sort); do
    case "$f" in
        *porting/common.c|*.h) continue ;;
    esac

    if ! printf '%s\n' "$BUILT" | grep -qxF "$f"; then
        skipped="$skipped $(basename "$f")"
        continue
    fi

    checked=$((checked + 1))
    if ! grep -q "common_ingame_overlay" "$f"; then
        echo "  FAIL $f takes the shortcuts and never draws the overlay --"
        echo "       volume and brightness change with nothing on screen to say so"
        rc=1
    fi
done

if [ "$checked" -eq 0 ]; then
    echo "SKIP: no built core calls common_emu_input_loop on this branch"
    exit 0
fi

[ -n "$skipped" ] && echo "       (not built, not checked:$skipped)"

if [ "$rc" -eq 0 ]; then
    echo "  OK   all $checked built core(s) draw the shortcut overlay"
    echo
    echo "PASSED"
else
    echo
    echo "FAILED"
fi
exit "$rc"
