#!/bin/bash
# scripts/check_xip_sentinels.py must pass the built ELF and must FAIL once an
# instruction word inside segacd's XIP code reads as a sentinel address (RED
# against the real image: one word of a copy is overwritten).
set -u
ELF=${ELF:-build/gw_retro_go.elf}
DUMP=${DUMP:-arm-none-eabi-objdump}
command -v "$DUMP" >/dev/null && [ -f "$ELF" ] || { echo "  SKIP no $ELF or $DUMP"; exit 0; }
"$DUMP" -h "$ELF" | grep -q " .xip_segacd " || { echo "  SKIP no Sega CD XIP in $ELF"; exit 0; }
rc=0
if python3 scripts/check_xip_sentinels.py "$DUMP" "$ELF" >/dev/null; then
  echo "  OK   built image: no instruction inside the sentinel window"
else
  echo "  FAIL built image has an instruction the sentinel patcher would corrupt"; rc=1
fi
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
# A word inside PsndRerate (cold, stays in the XIP): find its file offset.
read -r vma off <<<"$("$DUMP" -h "$ELF" | awk '$2==".xip_segacd"{print $4, $6}')"
sym=$("$DUMP" -t "$ELF" | awk '$NF=="segacd__PsndRerate"{print $1; exit}')
[ -n "$sym" ] || { echo "  SKIP segacd__PsndRerate not in .xip_segacd"; exit $rc; }
fo=$(( 0x$off + 0x$sym - 0x$vma + 8 ))
fo=$(( fo & ~3 ))
cp "$ELF" "$tmp/bad.elf"
printf '\x23\x01\x00\xdf' | dd of="$tmp/bad.elf" bs=1 seek=$fo conv=notrunc status=none
# RED must fail for THIS reason, not because the checker crashed.
if python3 scripts/check_xip_sentinels.py "$DUMP" "$tmp/bad.elf" 2>&1 | grep -q "holds 0xdf000123"; then
  echo "  OK   an instruction word of 0xdf000123 fails the check, by name"
else
  echo "  FAIL an instruction word of 0xdf000123 was not caught"; rc=1
fi
exit $rc
