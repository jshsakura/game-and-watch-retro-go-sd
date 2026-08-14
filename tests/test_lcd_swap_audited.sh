#!/usr/bin/env bash
# Nobody may add or move an lcd_swap() without deciding what it means for the
# drawn-frame counter.
#
# g_common_drawn_frames is incremented in lcd_swap(). That makes the DEFAULT
# honest -- ~68 call sites funnel through one function, so a new core is counted
# without knowing the counter exists -- and it makes exactly one mistake
# possible: skip the render, flip anyway, and every frame reports as drawn.
# Three cores do flip anyway, on purpose (holding the flip leaves the panel on a
# front buffer that goes stale), and they call lcd_swap_stale() instead. The
# counter this replaced had the mirror-image defect and put Virtual Boy in the
# ledger at 9 drawn fps while it was presenting 36.
#
# Both mistakes produce a number that looks like a measurement. Neither produces
# a compile error, a crash, or a visible glitch. So the check is a census:
# tests/lcd_swap_audited.txt lists every file that calls either function, with
# its counts and the verdict from reading its loop. When the tree and the list
# disagree, someone changed a flip and this says so.
#
# Deliberately NOT a heuristic. The obvious gate -- "flag any file that gates
# its render with if (drawFrame) and also calls lcd_swap()" -- flags a7800,
# amstrad, ngp, pkmini, wsv, wswan and nes_fceu, all of which are correct:
# their swap is INSIDE the guard. Telling those apart needs a parser, and a
# gate that cries wolf on seven correct cores is a gate people learn to skip.
# Counting is exact.
#
# Deliberately NO external tools. Five gates in this tree were SKIPPING in every
# CI release because nm and objdump were not on the container's PATH, including
# the cross-overlay alias check that CLAUDE.md calls the only thing standing
# between a core and another core's symbols. Nobody noticed, because locally
# they passed. This runs on grep, sed and awk.
#
# WHY THE COMMENT STRIPPER AND NOT A REGEX. The first version matched
# `^[^*/]*lcd_swap\(` -- this tree's idiom for "a call, not the name in prose",
# borrowed from test_ingame_overlay_wired.sh. It is wrong, and wrong in the
# direction that makes a gate worthless: it drops any line with a * or a / ahead
# of the call.
#
#     if (*flag) lcd_swap();          <-- missed. Ordinary C.
#     if (a / b) lcd_swap();          <-- missed.
#     if (scale * 2) lcd_swap();      <-- missed.
#     printf("...lcd_swap()...");     <-- counted. A string literal.
#
# Four out of six wrong on a probe file. The under-count is the serious half:
# file DISCOVERY used the same pattern, so a new core whose only flip sat on
# such a line was never scanned at all -- not listed, not missed, invisible.
# The `checked < 30` floor could not catch it either, because the other 44 files
# keep the count up. A gate that passes while seeing nothing is the 0727 failure
# again, in a new costume.
#
# So: strip comments and string literals properly (awk, character by character,
# tracking block-comment and string state across lines), then count. The
# self-check below runs that stripper against a fixture built from the exact
# lines the old pattern got wrong, and fails if any of them is miscounted.
#
# What it still cannot see: gw_firmware_abi.c hands lcd_swap out as a function
# pointer, and an external core calling through it is not in this tree. That
# gap is written down at the bottom of the audited list.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

LIST="tests/lcd_swap_audited.txt"
ROOTS="Core/Src Core/Inc retro-go-stm32"

echo "=== every lcd_swap() call site has been audited for the drawn counter ==="

if [ ! -f "$LIST" ]; then
    echo "  FAIL $LIST is missing -- the census has no baseline to compare against"
    echo
    echo "FAILED"
    exit 1
fi

rc=0

# Emit the file with /* */ comments, // comments and "string literals" removed.
# State carries across lines, which is the whole point: a block comment's
# continuation lines are only recognisable from the /* that opened it.
STRIP='
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

counts() {  # file -> "swap=N stale=N"
    local stripped s t
    # CH is the single quote, passed in as a variable: a ' cannot appear inside
    # the single-quoted STRIP program above. It tracks CHARACTER literals, and
    # it is not a nicety -- without it a plain `char q = '"'` turned on the
    # string state and swallowed the rest of the file, which under the old
    # discovery made the file vanish from the census entirely. Six files in
    # scope contain exactly that literal.
    stripped="$(awk -v CH="'" "$STRIP" "$1")"
    s="$(printf '%s' "$stripped" | grep -o 'lcd_swap('       | wc -l | tr -d ' ')"
    t="$(printf '%s' "$stripped" | grep -o 'lcd_swap_stale(' | wc -l | tr -d ' ')"
    echo "swap=$s stale=$t"
}

# --- self-check: the stripper must survive the lines the old pattern lost ----
# This is not ceremony. The defect it pins was invisible in every other way: the
# suite was green, the census was complete, and the gate saw two of six calls.
FIX="$(mktemp)"; trap 'rm -f "$FIX"' EXIT
cat > "$FIX" <<'FIXTURE'
    static char csv_quote = '"';
    printf("Calling lcd_swap() now\n");
    if (scale * 2) lcd_swap();
    if (*flag) lcd_swap();
    if (a / b) lcd_swap();
    if (drawFrame) { blit(); lcd_swap(); }
    /* Panel behaviour is identical to lcd_swap();
     * only the bookkeeping differs -- must NOT count. */
    p = a/b; lcd_swap_stale();
    x = 1; // trailing lcd_swap() in a line comment
FIXTURE
got="$(counts "$FIX")"
if [ "$got" != "swap=4 stale=1" ]; then
    echo "  FAIL the matcher itself is broken: fixture counted [$got], expected [swap=4 stale=1]"
    echo "       4 real calls (two behind a *, one behind a /, one plain), 1 stale call,"
    echo "       and it must NOT count the string literal, the block comment or the"
    echo "       line comment. Every census result below is meaningless until this passes."
    rc=1
fi

# --- the tree, as it is now -------------------------------------------------
# FAIL CLOSED. Every file whose raw text merely MENTIONS lcd_swap is reported,
# including the ones that only talk about it -- those are listed as `prose` with
# swap=0 stale=0. The earlier version dropped anything counting zero, which
# meant a stripper bug that zeroed a file's counts erased it from the census
# instead of failing it: a char literal was enough, and the run stayed green.
# Now zero is a value like any other and has to match the audit.
#
# -Z / read -d '' rather than $(...) word splitting: a path with a space made
# the previous loop pass fragments to awk, which failed, which counted zero,
# which dropped the file -- fail-open again, from a space.
actual="$(
    grep -rlZ 'lcd_swap' $ROOTS \
         --include='*.c' --include='*.cpp' --include='*.cxx' --include='*.cc' \
         --include='*.h' --include='*.hpp' 2>/dev/null \
    | sort -z \
    | while IFS= read -r -d '' f; do echo "$f $(counts "$f")"; done
)"

# The audited list is whitespace-separated, so it cannot express a path with a
# space in it -- and the comparison below would silently read such a path as its
# first word. Say that plainly instead of half-handling it.
spaced="$(grep -rlZ 'lcd_swap' $ROOTS \
              --include='*.c' --include='*.cpp' --include='*.cxx' --include='*.cc' \
              --include='*.h' --include='*.hpp' 2>/dev/null \
          | tr '\0' '\n' | grep ' ' || true)"
if [ -n "$spaced" ]; then
    echo "  FAIL these paths contain a space, which $LIST cannot represent:"
    printf '       %s\n' $spaced
    echo "       Rename the directory or file. The census compares whitespace-"
    echo "       separated fields and would otherwise match on the first word."
    rc=1
fi

# --- the list, as it was audited --------------------------------------------
expected="$(sed -e 's/#.*//' -e 's/[[:space:]]\+$//' "$LIST" \
            | grep -vE '^[[:space:]]*$' \
            | awk '{print $1, $2, $3}' | sort)"

# --- compare ----------------------------------------------------------------
checked=0
while read -r path swap stale; do
    [ -n "${path:-}" ] || continue
    checked=$((checked + 1))
    want="$(printf '%s\n' "$expected" | awk -v p="$path" '$1==p {print $2, $3; exit}')"
    if [ -z "$want" ] && [ "$swap $stale" = "swap=0 stale=0" ]; then
        echo "  FAIL $path mentions lcd_swap and is not in $LIST"
        echo "       It calls neither function -- the name appears only in a comment"
        echo "       or a string. Add it as \`swap=0 stale=0 prose\`. Listing these is"
        echo "       what makes the census fail CLOSED: if a parser bug ever zeroes a"
        echo "       real file's counts, it mismatches instead of disappearing."
        rc=1
    elif [ -z "$want" ]; then
        echo "  FAIL $path calls lcd_swap() and is not in $LIST"
        echo "       Read its loop: when it SKIPS a frame, does new content still"
        echo "       reach the back buffer? Always -> lcd_swap() is right and"
        echo "       drawn == emu is the truth. Never -> lcd_swap_stale() on the"
        echo "       skip path. Sometimes -> the condition is the core's own."
        echo "       Then add the line, with which of those it is."
        rc=1
    elif [ "$want" != "$swap $stale" ]; then
        echo "  FAIL $path is audited as [$want] but the tree has [$swap $stale]"
        echo "       A flip was added, removed, or changed between counted and"
        echo "       uncounted. Re-read the loop and update $LIST."
        rc=1
    fi
done <<< "$actual"

# The other direction: a line for a file that no longer calls either function is
# a stale audit, and a stale audit is how a real core hides behind a green run.
while read -r path swap stale; do
    [ -n "${path:-}" ] || continue
    if ! printf '%s\n' "$actual" | awk -v p="$path" '$1==p {found=1} END {exit !found}'; then
        echo "  FAIL $LIST lists $path ($swap $stale) but nothing there calls lcd_swap()"
        echo "       The file moved or the call went away -- drop the line."
        rc=1
    fi
done <<< "$expected"

# The scope IS the check. A green run over three files proves nothing, and a
# broken grep fails open, so say so loudly rather than quietly.
if [ "$checked" -lt 30 ]; then
    echo "  FAIL only $checked call site(s) matched -- the scan is broken, not the tree"
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "  OK   all $checked file(s) that flip the panel are accounted for"
    echo
    echo "PASSED"
else
    echo
    echo "FAILED"
fi
exit "$rc"
