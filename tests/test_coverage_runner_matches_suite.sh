#!/bin/bash
# tests/coverage.sh must actually produce data for every file the scope calls
# MEASURED. When it does not, a file with a working test is reported as having
# no harness at all and the work order grows items that are already done: that
# is what happened to music_id3.c (74.6% covered, listed as "no host harness
# exists yet") and music_lyrics.c (91.9%), while md32x_border_clear.c was not
# in the denominator at all.
#
# This checks the OUTPUT, not the script. An earlier version grepped
# coverage.sh for each source path and failed five healthy files, because
# rg_alarm.c and friends are built through a test that whole-file #includes
# them under an unrelated name (test_alarm). Grepping a build script for a
# filename is a proxy; the report is the thing itself.
#
# coverage_report.py already prints the offenders under "MEASURED in scope but
# NO coverage data collected this run". The gate is that this section is empty.
#
# Reads a log if given one ($1), otherwise runs tests/coverage.sh itself.
# Exits 0 (pass) or 1 (fail). Skips loudly if it cannot get a report.
set -u
cd "$(dirname "$0")/.."

LOG="${1:-}"
if [ -z "$LOG" ]; then
  command -v gcov >/dev/null 2>&1 || { echo "SKIP gcov not available"; exit 0; }
  LOG=$(mktemp)
  if ! bash tests/coverage.sh >"$LOG" 2>&1; then
    echo "SKIP tests/coverage.sh did not complete -- nothing to check"
    exit 0
  fi
fi
[ -s "$LOG" ] || { echo "SKIP empty coverage log"; exit 0; }

grep -q "TOTAL (MEASURED modules only)" "$LOG" || {
  echo "SKIP coverage log has no TOTAL line -- the run did not get that far"; exit 0; }

offenders=$(awk '
  /MEASURED in scope but NO coverage data/ { on = 1; next }
  on && /^  - / { print substr($0, 5) }
  on && !/^  - / && NF { on = 0 }
' "$LOG")

if [ -n "$offenders" ]; then
  echo "  FAIL these files are MEASURED in tests/coverage_scope.txt but produced no data:"
  echo "$offenders" | sed 's/^/       /'
  echo "       Either make tests/coverage.sh build them, or mark them UNMEASURED"
  echo "       with a reason. A denominator that lies in either direction is worse"
  echo "       than no number."
  exit 1
fi

n=$(grep -cE '^\S+\.c +[0-9]+/[0-9]+' "$LOG" || true)
echo "  OK   every MEASURED file produced coverage data ($n rows in the report)"

# The other direction, and the one that actually bit: a file marked UNMEASURED
# ("no host harness exists yet") that tests/run.sh already links. That does not
# understate a number, it invents work -- music_id3.c sat in the work order at
# 74.6% covered, and the next person to pick it up would have written a second
# test for a file that had one.
SCOPE=tests/coverage_scope.txt
SUITE=tests/run.sh
phantom=0
if [ -f "$SCOPE" ] && [ -f "$SUITE" ]; then
  while IFS=$'\t' read -r status path _rest; do
    [ "${status:-}" = "UNMEASURED" ] || continue
    [ -n "${path:-}" ] || continue
    base=$(basename "$path")
    # run.sh builds several sources through a $VAR prefix, so match the file
    # name rather than the path. A comment mentioning it does not count.
    if grep -E "^[^#]*\b${base//./\\.}\b" "$SUITE" | grep -q "\$CC\|gcc\|#include"; then
      echo "FAIL $path is UNMEASURED (\"no harness yet\") but tests/run.sh compiles it"
      phantom=$((phantom + 1))
    fi
  done < "$SCOPE"
fi
if [ "$phantom" -gt 0 ]; then
  echo "       Mark it MEASURED and add it to tests/coverage.sh. Leaving it in the"
  echo "       work order sends the next person to write a test that already exists."
  exit 1
fi
echo "  OK   no UNMEASURED file is already compiled by the suite"
exit 0
