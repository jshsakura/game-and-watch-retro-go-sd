#!/usr/bin/env bash
# TamaPoke's save has to be in the right place and be asked for at every exit.
#
# Two faults shipped, and neither is something a unit test of the save code could
# have seen, because the save code was fine:
#
#   - It wrote to "/tamapoke.sav" -- the ROOT of the SD card, next to the user's
#     own folders. There is a rule for where a save goes and every other homebrew
#     follows it: Zelda 3, Super Mario World and Super Metroid all call
#     odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path), which
#     yields /data/<rom path under /roms>.sram. Reported from hardware as "why is
#     the TamaPoke save sitting in the root?".
#   - The store lives in RAM and is flushed at safe points, and the safe points are
#     hooks the launcher ASKS THE CORE FOR: odroid_system_emu_init()'s shutdown
#     and sram_save. Both were NULL. sram_save is called on every sleep/standby
#     entry AND inside odroid_system_switch_app() before the card is unmounted;
#     shutdown is called on power-off. So quitting to the launcher, holding POWER,
#     and the low-battery auto-off each lost everything since the last commit --
#     while pet.save() and tamapoke_prefs_commit() worked perfectly.
#
# This is a wiring test (see CLAUDE.md, "the bug is usually in the thing that never
# got wired"): it asserts the contract at the call site, which is the only place
# either fault is visible.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

SHIM="$REPO/Core/Src/porting/tamapoke/tamapoke_shim.cpp"
MAIN="$REPO/Core/Src/porting/tamapoke/main_tamapoke.cpp"

echo "=== tamapoke: the save goes where the launcher puts every other one ==="

rc=0
fail() { echo "  FAIL $1"; rc=1; }
ok()   { echo "  OK   $1"; }

for f in "$SHIM" "$MAIN"; do
    if [ ! -f "$f" ]; then
        echo "SKIP: $f not present"
        exit 0
    fi
done

# 1. The path must come from the launcher, not from a literal in this file. Any
#    absolute path ending in a save-ish extension is the shipped bug.
if grep -qE '"/[A-Za-z0-9_.-]+\.(sav|sram|dat)"' "$SHIM" \
   && ! grep -q 'SAVE_PATH_LEGACY' "$SHIM"; then
    fail "a hardcoded absolute save path in tamapoke_shim.cpp"
fi
if ! grep -q 'odroid_system_get_path(ODROID_PATH_SAVE_SRAM' "$SHIM"; then
    fail "the save path is not built with odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ...) --"\
"that is the rule Zelda 3 / SMW / Super Metroid all follow"
else
    ok "the path comes from odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ...)"
fi
if ! grep -q 'ACTIVE_FILE' "$SHIM"; then
    fail "the path is not keyed on ACTIVE_FILE, so it is not this ROM's save"
else
    ok "keyed on ACTIVE_FILE, like every other core"
fi

# 2. The old location must still be read, or moving it loses the player's pet.
if ! grep -q 'SAVE_PATH_LEGACY' "$SHIM"; then
    fail "nothing reads the old root path -- moving the save orphans every existing pet"
elif ! grep -q 'prefs_read_from(SAVE_PATH_LEGACY)' "$SHIM"; then
    fail "SAVE_PATH_LEGACY is defined but never read"
elif ! grep -q 'odroid_sdcard_unlink(SAVE_PATH_LEGACY)' "$SHIM"; then
    fail "the old root file is read but never removed -- it stays in the user's root for ever"
else
    ok "the old root path is read once, then removed"
fi

# 3. And the directory has to exist before fopen, on a card that never had one.
if ! grep -q 'odroid_sdcard_mkdir' "$SHIM"; then
    fail "nothing creates the save directory -- fopen will not, and a fresh card has no /data"
else
    ok "the save directory is created before the first write"
fi

echo
echo "=== tamapoke: every exit the launcher offers must commit the save ==="

# 4. odroid_system_emu_init's shutdown (4th) and sram_save (6th) must be non-NULL.
#    Read the call as one line so a wrapped call is still checked, and pick the
#    occurrence that passes handlers -- the name also appears in the comment above
#    it, and matching that first reported "this call passes 1 handler".
call="$(tr '\n' ' ' < "$MAIN" \
        | grep -oE 'odroid_system_emu_init[[:space:]]*\([^;]*\)' \
        | grep '&' | head -1)"
if [ -z "$call" ]; then
    fail "main_tamapoke.cpp never calls odroid_system_emu_init -- Save/Load in the menu do nothing"
else
    args="${call#*(}"; args="${args%)*}"
    IFS=',' read -r -a a <<< "$args"
    trim() { printf '%s' "$1" | tr -d ' \t'; }
    n="${#a[@]}"
    if [ "$n" -ne 7 ]; then
        fail "odroid_system_emu_init takes 7 handlers, this call passes $n"
    else
        if [ "$(trim "${a[3]}")" = "NULL" ]; then
            fail "shutdown_cb is NULL -- power-off and the low-battery auto-off lose the pet"
        else
            ok "shutdown_cb is wired ($(trim "${a[3]}"))"
        fi
        if [ "$(trim "${a[5]}")" = "NULL" ]; then
            fail "sram_save_cb is NULL -- quitting to the launcher and sleeping lose the pet"
        else
            ok "sram_save_cb is wired ($(trim "${a[5]}"))"
        fi
    fi
fi

# 5. Both hooks must actually reach the commit, not just be non-NULL.
for cb in tamapoke_shutdown tamapoke_sram_save; do
    body="$(sed -n "/static void $cb(void)/,/^}/p" "$MAIN")"
    if [ -z "$body" ]; then
        body="$(grep -E "static void $cb\(void\).*\{.*\}" "$MAIN" || true)"
    fi
    if ! printf '%s' "$body" | grep -q 'tamapoke_prefs_commit'; then
        fail "$cb does not call tamapoke_prefs_commit"
    else
        ok "$cb commits the store"
    fi
done

echo
if [ "$rc" -ne 0 ]; then
    echo "FAILED"
else
    echo "PASSED"
fi
exit "$rc"
