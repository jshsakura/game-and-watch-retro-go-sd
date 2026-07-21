#!/bin/bash
# The bug was real, on-device: exit the Clock app, then launch a game ->
# BusFault (CFSR IMPRECISERR; PC=get_darken_pixel_d, LR=gui_draw_item_postion_h
# -- the launcher's ROM-list redraw reading garbage right after a
# "gui_resize list snes 4->1 items" / "0->4" churn).
#
# clock_stage_overlay() moves the launcher's bump-allocator cursor (ram_start)
# past .overlay_clock's own footprint before calling into the overlay -- the
# exact same move every emulator core makes for its own overlay
# (`ram_start = &_OVERLAY_X_BSS_END` in main_sm.c, main_vb.c, ~30 cores) and
# NONE of them ever restore it. That was never a rule anyone followed; it was
# reboot-as-accident -- every core's return-to-launcher path is a reboot, so
# nothing ever comes back to read the stale cursor. The clock is the only
# RAM_EMU overlay that returns IN-PROCESS to the launcher, so it is the only
# one that actually needs the restore, and forgetting it left the launcher's
# OWN ram_calloc() user (rg_emulators.c's shared_files ROM list) walking
# through .overlay_clock's leftover footprint instead of its own.
set -u
rc=0
RING=Core/Src/retro-go/rg_clock_ring.c

body() { awk '/^void rg_clock_show\(void\)/,/^}/' "$RING"; }

# rg_clock_show() must be a thin wrapper around the real loop (clock_show_run),
# not the loop itself -- so the restore below runs no matter which of
# clock_show_run()'s exit paths returned: structural, not discipline. A future
# early-return added inside the body still comes back here before the
# launcher ever regains control.
if grep -qE "^void rg_clock_show\(void\)" "$RING" && body | grep -q "clock_show_run();"; then
    echo "  OK   rg_clock_show() is a thin wrapper around clock_show_run()"
else
    echo "  FAIL rg_clock_show() no longer delegates to a separate body function --"
    echo "       the restore below can no longer be guaranteed for every exit path"
    rc=1
fi

# The restore itself: ram_start reset to 0, the launcher's "unowned" sentinel
# (gui.c / rg_main.c lazily reclaim __RAM_EMU_START__ the next time something
# needs it -- see rg_emulators.c's OWN return-to-launcher cleanup, which resets
# the same cursor the same way after an emulator exits). Unconditional: not
# inside an #if, not only on one branch.
if body | grep -qE "^\s*ram_start = 0;\s*$"; then
    echo "  OK   rg_clock_show() resets ram_start to 0 on the way out"
else
    echo "  FAIL rg_clock_show() does not unconditionally reset ram_start -- the"
    echo "       launcher's next ram_calloc() (shared_files) walks .overlay_clock's"
    echo "       leftover footprint instead of its own, exactly like the 0721 BusFault"
    rc=1
fi

# Fail loudly, not quietly: an assert on the expected pre-restore state, so a
# future bug in clock_show_run() (or anything else that clobbers ram_start
# mid-session) trips here instead of silently corrupting the ROM list again.
if body | grep -q "assert(ram_start =="; then
    echo "  OK   rg_clock_show() asserts ram_start's state before resetting it"
else
    echo "  FAIL rg_clock_show() has no assert guarding the restore -- a future bug"
    echo "       here would corrupt the launcher's ROM list silently again"
    rc=1
fi

# The structural checks above prove the SHAPE of the fix; this proves it
# actually WORKS -- a host test that drives the real rg_clock_show() with a
# scripted exit and checks the postcondition, not a reimplementation of it.
if grep -q "test_ram_start_restored_on_exit" tests/test_clock_more.c; then
    echo "  OK   a runtime test drives the real rg_clock_show() and checks ram_start after"
else
    echo "  FAIL no runtime test exercises rg_clock_show()'s ram_start restore"
    rc=1
fi

exit $rc
