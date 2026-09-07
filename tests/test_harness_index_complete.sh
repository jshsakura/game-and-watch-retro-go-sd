#!/bin/bash
# docs/HARNESSES.md calls itself the catalogue of every harness. On 2026-09-07
# it named nine of the twenty-four gates in tests/ and none of two harness
# directories. An index that omits most of its entries is worse than no index:
# it answers "is there already a test for this?" with a confident no.
#
# So the catalogue is checked rather than maintained by memory. Two rules:
#   1. every tests/*.sh gate is named in docs/HARNESSES.md
#   2. every tools/*harness*, tools/*_rig, tools/gnw_probe, tools/gba_m4a is too
#
# run.sh and coverage.sh are the runners, not entries, and are exempt.
#
# Exits 0 (pass) or 1 (fail). Skips loudly if the index is missing.
set -u
cd "$(dirname "$0")/.."

# --red: drop one gate from a copy of the index and require this gate to name it.
if [ "${1:-}" = "--red" ]; then
  red=$(mktemp -d); trap 'rm -rf "$red"' EXIT
  mkdir -p "$red/tests" "$red/docs" "$red/tools"
  cp tests/*.sh "$red/tests/" 2>/dev/null
  cp -r tools/* "$red/tools/" 2>/dev/null
  sed 's|test_remove_extension\.sh|test_gone_from_the_index.sh|' \
      docs/HARNESSES.md > "$red/docs/HARNESSES.md"
  out=$(cd "$red" && bash "tests/$(basename "$0")" 2>&1); rc=$?
  [ "$rc" -ne 0 ] || { echo "  FAIL RED: an index missing an entry PASSED"; exit 1; }
  case "$out" in
    *"tests/test_remove_extension.sh is not named in"*) ;;
    *) echo "  FAIL RED: it failed, but did not name the missing entry:"
       echo "$out" | grep -E "FAIL" | head -3 | sed 's/^/         /'; exit 1 ;;
  esac
  echo "  OK   RED: an index that drops a gate is caught, by name"
  exit 0
fi

INDEX=docs/HARNESSES.md
[ -f "$INDEX" ] || { echo "SKIP $INDEX not found"; exit 0; }

missing=0
checked=0

for f in tests/*.sh; do
  [ -e "$f" ] || continue
  b=$(basename "$f")
  case "$b" in run.sh|coverage.sh) continue ;; esac
  checked=$((checked + 1))
  grep -qF "$b" "$INDEX" || { echo "FAIL $f is not named in $INDEX"; missing=$((missing + 1)); }
done

for d in tools/*harness* tools/*_rig tools/gnw_probe tools/gba_m4a; do
  [ -d "$d" ] || continue
  b=$(basename "$d")
  checked=$((checked + 1))
  grep -qF "$b" "$INDEX" || { echo "FAIL $d is not named in $INDEX"; missing=$((missing + 1)); }
done

if [ "$checked" -eq 0 ]; then
  echo "SKIP found no gates or harnesses to check"
  exit 0
fi
echo "  OK   $INDEX names all $checked gates and harnesses"
if [ "$missing" -gt 0 ]; then
  echo "       Add an entry saying what it proves and which accident it pins."
  echo "       A catalogue that answers \"is there already a test for this?\""
  echo "       with a wrong no is how the same test gets written twice."
  exit 1
fi
exit 0
