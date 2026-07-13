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

# The clock's own idle-dim/night-off logic (clock_should_dim/clock_dim_level/
# CLOCK_DIM_*/clock_backlight_t) was ripped out in full — it duplicated this
# same global rule, badly (a "don't dim while charging" exception that pinned
# the backlight lit whenever bq24072_get_state() misreported CHARGING with no
# charger attached). Both must stay gone: a second idle-power constant of the
# clock's own, or the bq24072 read that broke it, is the same class of bug
# creeping back in a new shape.
CLOCK=Core/Src/retro-go/rg_clock.c
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
# forever they set the global to off, same as everywhere else.
if grep -qE "s_keep_awake|keepawake|cb_keepawake|s_Clock_Keep_Awake" "$CLOCK"; then
    echo "  FAIL rg_clock.c has its own power opt-out again (s_keep_awake or similar)"
    rc=1
else
    echo "  OK   rg_clock.c has no opt-out of its own — the global timeout is the only rule"
fi

exit $rc
