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

# Strip /* */ and // comments, and "string"/'char' literals, carrying state
# across lines. CH is the single quote, passed in as a variable because it
# cannot appear inside this single-quoted program.
APPID_STRIP='
{
  line = $0; out = ""; i = 1; n = length(line)
  while (i <= n) {
    two = substr(line, i, 2); one = substr(line, i, 1)
    if (inblock)          { if (two == "*/") { inblock = 0; i += 2 } else i++ }
    else if (instr)       { if (one == "\\") i += 2
                            else { if (one == "\"") instr = 0; i++ } }
    else if (inchr)       { if (one == "\\") i += 2
                            else { if (one == CH) inchr = 0; i++ } }
    else if (two == "/*") { inblock = 1; i += 2 }
    else if (two == "//") { break }
    else if (one == "\"") { instr = 1; i++ }
    else if (one == CH)   { inchr = 1; i++ }
    else                  { out = out one; i++ }
  }
  print out
}'

appid_value() {
    [ -n "${1:-}" ] || return 0
    [ -f "$APPID_H" ] || return 0

    # Comments are stripped PROPERLY, not by "does the line start with * or /".
    # That shortcut returned confidently wrong numbers on four ordinary header
    # shapes -- an old value in a line-tail comment, a block comment whose body
    # lines happen not to start with *, a // comment, and a #if 0 block all made
    # it answer 99 or 7 where the compiler says 25. The one defence it seemed to
    # have (appid.h's retired 32X slot) worked only because that comment happens
    # to begin every line with a *; one reflow and it lied. Leaving an old number
    # in a comment beside a retired slot is exactly what that header invites.
    #
    # AMBIGUITY IS NOT AN ANSWER. Every match is collected and the value is
    # returned only if they all agree. Two different values for one name -- a
    # #if 0 alternative, a duplicate -- means this cannot know which the compiler
    # picked, so it says nothing and the caller labels the window "unknown".
    #
    # The `|| true` and the trailing `return 0` are not decoration. A name that is
    # absent makes grep exit 1, and under a caller running `set -e -o pipefail` --
    # which drawn_ab.sh and every probe script here does -- that failure propagates
    # out of the command substitution and KILLS THE RUN. A scene label would then
    # be able to abort a device measurement that was about to produce data. Caught
    # by tests/test_appid_value.sh, which is why that test exists.
    local v
    v="$(awk -v CH="'" "$APPID_STRIP" "$APPID_H" \
         | grep -oE "(^|[^A-Za-z0-9_])APPID_$1[[:space:]]*=[[:space:]]*[0-9]+" \
         | grep -oE '[0-9]+$' \
         | sort -u)" || v=""

    # More than one distinct value, or none: unknown.
    [ "$(printf '%s\n' "$v" | grep -c .)" = 1 ] && printf '%s\n' "$v"
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
