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

exit $rc
