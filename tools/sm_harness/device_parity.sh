#!/bin/bash
# Link the Super Metroid core with EXACTLY the files the device build uses, and
# nothing else. Any symbol left undefined is one the firmware link would have
# resolved silently to another overlay core's — which is how this port shipped
# three builds that drove the SNES bus through Super Mario World's g_snes.
#
# The main harness compiles the whole of external/sm, including sm_cpu_infra.c,
# which defines and sets g_snes. That is why it ran 4,000 clean frames while the
# device died on its first register read. A harness has to link the same program.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/sm_parity}

# The device's file list, straight from the Makefile — never a copy of it.
SRCS=$(make -pn 2>/dev/null | grep '^SM_C_SOURCES = ' | head -1 |
       sed 's/^SM_C_SOURCES = //; s|$(CORE_SM)|external/sm|g' | tr ' ' '\n' |
       grep '\.c$' | grep -v 'main_sm\.c$')   # the glue is firmware-only; stubbed below

n=$(echo "$SRCS" | wc -l)
echo "  device SM sources: $n (main_sm.c excluded — its symbols are stubbed here)"
echo "$SRCS" | grep -q sm_cpu_infra && { echo "  sm_cpu_infra.c is IN the device build?"; exit 1; }

gcc -c -Os -DNDEBUG -DTARGET_GNW -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim $SRCS -o /dev/null 2>/dev/null || true
mkdir -p "$OUT" && rm -f "$OUT"/*.o
for f in $SRCS; do
  gcc -c -Os -DNDEBUG -DTARGET_GNW -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim "$f" -o "$OUT/$(basename "${f%.c}").o"
done
gcc -c -Os -DTARGET_GNW -w -Iexternal/sm -Itools/sm_harness/shim tools/sm_harness/glue.c -o "$OUT/glue.o"

# Link. --unresolved-symbols=report-all so nothing is quietly deferred.
if ! gcc -o "$OUT/sm_parity" "$OUT"/*.o -lm 2>"$OUT/err.txt"; then
  echo
  echo "  FAIL: the device's source set does not link on its own."
  echo "        Every name below would be bound to ANOTHER CORE's overlay by the"
  echo "        firmware linker, silently, and read that core's address at runtime."
  echo
  grep -oE "undefined reference to \`[^']+'" "$OUT/err.txt" | sed "s/.*\`/        /;s/'//" | sort -u
  exit 1
fi
echo "  OK  the device's source set is symbol-complete"
