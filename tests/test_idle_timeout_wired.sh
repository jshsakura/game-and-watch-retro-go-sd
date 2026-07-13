#!/bin/bash
# The bug was not in the rule. It was that a loop never asked.
#
# "Idle power off" is one global setting. It was enforced in the launcher's home
# loop and in the in-game overlay — and the clock app, added later with a loop of
# its own, simply never called it. A bedside clock sat lit all night whatever the
# user had set, and nothing in the tree could tell. No unit test of the rule would
# ever have caught that, because the rule was fine.
#
# So pin the wiring: every screen that can sit idle must ask. The next app with its
# own loop will trip this on the way in, which is the only place it can be caught.
set -u
rc=0

check() {   # $1 = file, $2 = what it is
    if grep -q "odroid_idle_timeout_expired" "$1"; then
        echo "  OK   $2 honours the global idle timeout"
    else
        echo "  FAIL $2 has an idle loop and never asks odroid_idle_timeout_expired()"
        rc=1
    fi
}

check Core/Src/retro-go/rg_main.c       "the launcher home loop"
check Core/Src/porting/odroid_overlay.c "the in-game overlay loop"
check Core/Src/retro-go/rg_clock.c      "the clock app loop"

# The clock face's own loop asking (checked above) does not cover it: the
# clock has two MORE loops of its own -- clock_edit_time() (set date/time) and
# alarm_edit_view() (per-alarm time editor) -- that run their own for(;;)/
# while(true) instead of odroid_overlay_dialog(), and neither asked either.
# Either one could sit lit forever mid-edit if the user walked away.
CLOCK=Core/Src/retro-go/rg_clock.c
if grep -q "editor_idle_sleep_if_expired" "$CLOCK" \
   && [ "$(grep -c 'editor_idle_sleep_if_expired(&last_input' "$CLOCK")" -ge 2 ]; then
    echo "  OK   the clock's set-time and alarm-time editors both ask the idle timeout"
else
    echo "  FAIL clock_edit_time()/alarm_edit_view() have their own loops and don't both ask"
    rc=1
fi

# ...and the rule stays in one place: nobody re-derives it from the setting.
strays=$(grep -rln "MainMenuTimeoutS_get" Core/Src/ \
         | grep -vE "odroid_system\.c|odroid_settings\.c|rg_main\.c")
if [ -n "$strays" ]; then
    echo "  FAIL the timeout rule is being re-derived outside odroid_idle_timeout_expired():"
    echo "$strays" | sed 's/^/         /'
    rc=1
else
    echo "  OK   the rule lives in one place"
fi

# The user's complaint was never that the rule was wrong: it was that seeing
# or changing "Idle power off" meant leaving the clock for the launcher's
# settings. So the clock's PAUSE menu gained a row for it — but it must be
# rg_main.c's own main_menu_timeout_cb, reused, not a second implementation
# reading/writing odroid_settings_MainMenuTimeoutS_* on its own (that's just
# the "one place" bug again, one file over). Matches the row's own struct
# literal specifically (name immediately followed by the closing brace) --
# the extern declaration and this file's own comments about it also contain
# the bare name, so a plain grep for that alone would stay green even with
# the actual menu row deleted.
if grep -q "main_menu_timeout_cb}" "$CLOCK"; then
    echo "  OK   the clock's PAUSE menu exposes the launcher's Idle-power-off row"
else
    echo "  FAIL the clock's PAUSE menu no longer shows/edits the global Idle-power-off setting"
    rc=1
fi
if grep -qE "TIMEOUT_STEP|TIMEOUT_MIN|TIMEOUT_MAX" "$CLOCK"; then
    echo "  FAIL rg_clock.c reimplements the 60s-step timeout logic instead of reusing main_menu_timeout_cb"
    rc=1
else
    echo "  OK   rg_clock.c has no timeout-step logic of its own (it calls rg_main.c's row)"
fi
if grep -qE "^static[^(]*\bmain_menu_timeout_cb\b" Core/Src/retro-go/rg_main.c; then
    echo "  FAIL main_menu_timeout_cb is static again -- rg_clock.c can no longer link to it"
    rc=1
else
    echo "  OK   main_menu_timeout_cb has external linkage, so rg_clock.c can share it"
fi

# The clock's own idle-dim/night-off logic (clock_should_dim/clock_dim_level/
# CLOCK_DIM_*/clock_backlight_t) was ripped out in full — it duplicated this
# same global rule, badly (a "don't dim while charging" exception that pinned
# the backlight lit whenever bq24072_get_state() misreported CHARGING with no
# charger attached). Both must stay gone: a second idle-power constant of the
# clock's own, or the bq24072 read that broke it, is the same class of bug
# creeping back in a new shape.
if grep -q "bq24072" "$CLOCK"; then
    echo "  FAIL rg_clock.c reads bq24072 again — the idle-dim charging exception is back"
    rc=1
else
    echo "  OK   rg_clock.c does not touch bq24072 (no charging exception to misfire)"
fi
if grep -qE "CLOCK_DIM_IDLE_MS|CLOCK_DIM_FLOOR|clock_should_dim|clock_backlight_t|clock_dim_level|s_night_start" "$CLOCK"; then
    echo "  FAIL rg_clock.c has a second, clock-owned idle/dim timeout again"
    rc=1
else
    echo "  OK   the clock has no idle timeout of its own — only the global one"
fi

# The clock has NO power setting of its own, not even an opt-out. That was
# tried (s_keep_awake, an On/Off row that made the clock ignore the global
# timeout) and reverted: one setting, one place, zero exceptions. Whatever
# "Idle power off" says, the clock obeys — if a user wants the screen lit
# forever they set the global to off, same as everywhere else. (The
# main_menu_timeout_cb row added above is not this: it EDITS the one global
# value, in place, through the same function rg_main.c uses — it cannot make
# the clock stop obeying it, because there is only one value to obey.)
if grep -qE "s_keep_awake|keepawake|cb_keepawake|s_Clock_Keep_Awake" "$CLOCK"; then
    echo "  FAIL rg_clock.c has its own power opt-out again (s_keep_awake or similar)"
    rc=1
else
    echo "  OK   rg_clock.c has no opt-out of its own — the global timeout is the only rule"
fi

exit $rc
