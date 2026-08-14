# Read an APPID's numeric value out of Core/Inc/retro-go/appid.h, at run time.
#
# Source this; it defines appid_value().
#
#   . tools/gnw_probe/appid.sh
#   SNES_ID=$(appid_value SNES)      # -> 25, or EMPTY
#
# WHY NOT JUST WRITE 25. Because appid.h is positional in the way that matters:
# the enum sizes persistent_config_t's app[] array, so the file carries a
# standing warning that adding an entry resets every user's settings, and the
# values around a retired slot get renumbered by hand. A number pasted into a
# measurement script goes stale the next time somebody edits that header, and
# it goes stale SILENTLY -- the script keeps printing a label, just the wrong
# one. That is the same disease this was called in to cure: drawn_ab.sh picks
# its core by reading an address that belongs to the SNES overlay, which under
# any other core holds that core's data.
#
# WHAT THIS IS FOR, AND WHAT IT IS NOT FOR.
#
#   FOR: the scene label -- "savestate" vs "COLD BOOT (not the play scene)".
#        Being wrong here costs one line of prose. Being wrong is still bad
#        (this tree has paid three times for reading a cold-boot window as a
#        play scene), but it is not a wrong measurement.
#
#   NOT FOR: choosing which counter to read. Do not use it to decide between
#        g_snes_drawn_frames and g_common_drawn_frames -- read the shared pair
#        unconditionally. SNES flips inside its own `if (drawFrame)` block
#        (main_snes.c:1399-1404, one call site) and bumps g_snes_drawn_frames
#        on the same condition (:1361), so the two count the same event and the
#        choice buys nothing while a wrong answer costs a wrong number.
#
# BEST EFFORT, ON PURPOSE. Every failure path returns EMPTY and exits 0. The
# caller must treat empty as "unknown scene, carry on", never as a reason to
# stop: a safety net that fails the run is a net people learn to cut. Five
# gates in this tree spent months SKIPPING in CI for a missing tool and nobody
# noticed; the answer is to degrade loudly in the output, not to abort.
#
# Returns empty when: the header is missing, the name is absent, or the entry
# has no explicit `= N`. That last one matters -- an implicitly-numbered entry
# CAN be counted positionally, and this deliberately does not, because counting
# positions is exactly how a script quietly disagrees with the compiler.

APPID_H="${APPID_H:-Core/Inc/retro-go/appid.h}"

appid_value() {
    [ -n "${1:-}" ] || return 0
    [ -f "$APPID_H" ] || return 0

    # Drop comments before matching. Same idiom as tests/test_lcd_swap_audited.sh:
    # a line whose first non-space character is * or / is prose. appid.h's retired
    # 32X slot has a six-line block comment; without this its text is in scope.
    #
    # The `|| true` and the trailing `return 0` are not decoration. A name that is
    # absent makes grep exit 1, and under a caller running `set -e -o pipefail` --
    # which drawn_ab.sh and every probe script here does -- that failure propagates
    # out of the command substitution and KILLS THE RUN. A scene label would then
    # be able to abort a device measurement that was about to produce data. Caught
    # by tests/test_appid_value.sh, which is why that test exists.
    local v
    v="$(grep -v '^[[:space:]]*[*/]' "$APPID_H" \
         | grep -oE "\bAPPID_$1[[:space:]]*=[[:space:]]*[0-9]+" \
         | grep -oE '[0-9]+$' \
         | head -1)" || v=""

    [ -n "$v" ] && printf '%s\n' "$v"
    return 0
}

# Convenience for the caller's output line. Prints what was found, or says
# plainly that it could not tell -- an unlabelled window is honest, a
# confidently mislabelled one is not.
appid_describe() {
    local v
    v="$(appid_value "${1:-}")"
    if [ -n "$v" ]; then
        printf 'APPID_%s=%s\n' "$1" "$v"
    else
        printf 'APPID_%s=unknown (no explicit value in %s)\n' "${1:-?}" "$APPID_H"
    fi
}
