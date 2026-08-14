#!/usr/bin/env bash
# tools/gnw_probe/appid.sh reads an APPID's value out of the header instead of
# carrying a copy of it. Two properties are the whole point, and both are the
# kind that rot silently:
#
#   1. It never guesses. An entry without an explicit `= N` returns EMPTY. The
#      obvious "improvement" -- count positions like the compiler does -- is
#      how a script comes to disagree with the compiler about a header that
#      already warns, in its own comments, that its numbering is hand-managed
#      around a retired slot.
#
#   2. It never fails the caller. Missing header, unknown name, empty argument:
#      all return empty and exit 0. It labels a measurement window; a label
#      that can abort a device run is worse than no label.
#
# Neither property has a symptom. A regression in (1) prints a plausible wrong
# number; a regression in (2) kills a run that was about to produce data.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

echo "=== appid.sh: reads the header, guesses nothing, fails nothing ==="

if [ ! -f tools/gnw_probe/appid.sh ]; then
    echo "  FAIL tools/gnw_probe/appid.sh is missing"
    echo; echo "FAILED"; exit 1
fi

# shellcheck source=/dev/null
. tools/gnw_probe/appid.sh

rc=0
check() { # name, got, want
    if [ "$2" = "$3" ]; then
        echo "  OK   $1 -> [$2]"
    else
        echo "  FAIL $1 -> [$2], expected [$3]"
        rc=1
    fi
}

# Against the real header. SNES/SM are the interesting pair: a naive prefix
# match would return APPID_SMS (3) or APPID_SMW (12) for SM.
check "APPID_SNES"      "$(appid_value SNES)"      "25"
check "APPID_SM (not SMS/SMW)" "$(appid_value SM)" "23"
check "APPID_MD"        "$(appid_value MD)"        "8"
check "APPID_LAUNCHER"  "$(appid_value LAUNCHER)"  "0"
# The retired 32X slot is followed by a six-line block comment. If comment text
# ever enters the match, this is where it shows.
check "APPID_32X (past a block comment)" "$(appid_value 32X)" "26"

# Failure paths return empty, and return 0.
check "unknown name"    "$(appid_value NOSUCHCORE)" ""
check "empty argument"  "$(appid_value)"            ""

appid_value NOSUCHCORE >/dev/null; grc=$?
check "unknown name exit code" "$grc" "0"

MISSING_H="$APPID_H"
APPID_H=/nonexistent/appid.h
check "missing header"  "$(appid_value SNES)"       ""
appid_value SNES >/dev/null; grc=$?
check "missing header exit code" "$grc" "0"
APPID_H="$MISSING_H"

# Property (1), against a header edited to number an entry implicitly. The
# compiler would still call it 25; this must say it does not know rather than
# agree by arithmetic.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
sed 's/    APPID_SNES     = 25,/    APPID_SNES,/' "$APPID_H" > "$TMP/appid.h"
if ! grep -q '^[[:space:]]*APPID_SNES,' "$TMP/appid.h"; then
    echo "  FAIL the fixture did not apply -- appid.h's APPID_SNES line changed shape"
    echo "       (this test would otherwise pass without testing anything)"
    rc=1
else
    saved="$APPID_H"; APPID_H="$TMP/appid.h"
    check "implicit entry is not guessed"    "$(appid_value SNES)" ""
    check "explicit ones still readable"     "$(appid_value GBA)"  "24"
    APPID_H="$saved"
fi

if [ "$rc" -eq 0 ]; then
    echo; echo "PASSED"
else
    echo; echo "FAILED"
fi
exit "$rc"
