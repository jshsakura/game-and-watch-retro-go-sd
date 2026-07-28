#!/usr/bin/env bash
# The pet's animation must be chosen from the enum the sprite packs are indexed by.
#
# There are two: ACT_* (the port's own behaviour states, in tamapoke_ui.cpp) and
# PMD_* (the ids inside a TPK2 pack, in tamapoke_sprites.h). drawPmdAct() indexes
# the pack, so it takes PMD_*. The code passed ACT_*:
#
#   ACT_SLEEP  = 1 -> PMD_WALKL   a sleeping pet walked sideways under its own "Zz"
#   ACT_EAT    = 2 -> PMD_WALKR   feeding played walk-right, so "no eating animation"
#   ACT_HURT   = 3 -> PMD_SLEEP
#   ACT_POSE   = 4 -> PMD_EAT
#   ACT_NOD    = 5 -> PMD_HURT
#   ACT_BREATH = 6 -> PMD_ATTACK
#
# Only ACT_IDLE was right, because both enums start at 0 -- which is exactly why it
# read as odd animation choices for months instead of as a bug. No unit test of the
# blitter could see it: every id it was handed was a valid id.
#
# So this is a type-confusion gate, checked at the call site: nothing may pass an
# ACT_* value to drawPmdAct()/drawPmdActM().
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
UI="$REPO/Core/Src/porting/tamapoke/tamapoke_ui.cpp"

echo "=== tamapoke: pet animations are chosen from PMD_*, not ACT_* ==="

rc=0
fail() { echo "  FAIL $1"; rc=1; }
ok()   { echo "  OK   $1"; }

if [ ! -f "$UI" ]; then
    echo "SKIP: $UI not present"
    exit 0
fi

# 1. No ACT_* may appear as an argument to a PMD draw call. The mapping function is
#    the one legal place an ACT_* is read, and it is matched by name below.
bad="$(grep -nE 'drawPmdActM?\(' "$UI" | grep -E '\(\s*ACT_|,\s*ACT_' || true)"
if [ -n "$bad" ]; then
    fail "an ACT_* value is passed to a PMD draw call:"
    printf '       %s\n' "$bad"
else
    ok "no ACT_* reaches drawPmdAct()/drawPmdActM()"
fi

# 2. The mapping must exist and must cover every ACT_* the behaviour machine sets.
if ! grep -q 'static uint8_t pmdActFor' "$UI"; then
    fail "no pmdActFor() -- something has to translate the two enums"
else
    missing=""
    for a in ACT_SLEEP ACT_EAT ACT_HURT ACT_POSE ACT_NOD ACT_BREATH; do
        grep -qE "case $a:" "$UI" || missing="$missing $a"
    done
    if [ -n "$missing" ]; then
        fail "pmdActFor() does not map:$missing"
    else
        ok "pmdActFor() maps every non-idle behaviour state"
    fi
fi

# 3. And the choice must survive a pack that lacks the action. Measured over the 302
#    shipped packs: EAT is in 54, POSE 58, NOD 52, BREATH 50, while IDLE/WALKL/WALKR/
#    SLEEP/HURT/ATTACK/HOP are in all of them. Without a fallback that is actually
#    present, 82% of species silently play their idle cycle -- which is the SECOND
#    half of "there is no eating animation", after the enum fix.
if ! grep -q 'static uint8_t pickAct' "$UI"; then
    fail "no pickAct() -- an action a pack does not carry has to fall back to one it does"
elif ! grep -qE 'pickAct\(PMD_EAT, *PMD_HOP\)' "$UI"; then
    fail "eating has no fallback: PMD_EAT is present in only 54 of 302 packs"
else
    ok "a missing action falls back to one every pack carries"
fi

# 4. The walk cycles must be used for walking. They were unused entirely: the pet
#    slid to its target playing its idle animation, while sleep played walk-left.
if ! grep -qE 'PMD_WALKL' "$UI" || ! grep -qE 'PMD_WALKR' "$UI"; then
    fail "PMD_WALKL/PMD_WALKR are never used -- the wander plays no walk cycle"
else
    ok "the wander uses the pack's walk cycles"
fi

echo
if [ "$rc" -ne 0 ]; then echo "FAILED"; else echo "PASSED"; fi
exit "$rc"
