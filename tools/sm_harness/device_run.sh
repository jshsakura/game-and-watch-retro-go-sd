#!/bin/bash
# Run the sm core on the host with the DEVICE's compile-time reality:
#   -DTARGET_GNW  -> snes->apu is NULL (spc_player is the sound chip), the PPU
#                    renders RGB565, VRAM comes from itc_calloc.
#   the device's SM_C_SOURCES, and nothing else.
#
# The old harness compiled the whole tree without TARGET_GNW, so it had a real
# SPC700 and a real g_snes. It ran 4,000 clean frames while the device HardFaulted
# inside a 64 KB memcpy from snes->apu->ram — with snes->apu NULL. Same program, or
# it proves nothing.
#
# Same program is still not the same CPU. Two of the device's rules are forced on
# the host too, because the host breaks neither of them on its own:
#
#   -Werror=implicit-function-declaration
#       An implicit declaration returns int. On the 32-bit device a truncated
#       pointer is still the pointer; on a 64-bit host it is a wild address.
#       spc_player.c called ahb_malloc() with no prototype, and the harness died in
#       SpcPlayer_Create before it ever reached the emulator.
#
#   -fsanitize=alignment, then fail on the 64-bit violations only
#       Cortex-M7 traps an unaligned LDRD/STRD and nothing else: unaligned halfword
#       and word accesses are fine, and the SNES code does them by the dozen. So the
#       "requires 2/4 byte alignment" reports are noise here, and only "requires 8
#       byte alignment" is a fault the device would take. ClearBackdrop() stores 64
#       bits into a uint16 buffer that sat at 2 mod 4 — the device took a UsageFault
#       on the first rendered line while the host rendered 4,000 frames happily.
#
# Usage: device_run.sh [rom [frames]]   — with a ROM it also runs and gates on the above.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/sm_device_run}
mkdir -p "$OUT" && rm -f "$OUT"/*.o

DEVICE_RULES="-fsanitize=alignment -Werror=implicit-function-declaration"

# Extra -D flags, the same way the QEMU rigs take RIG_EXTRA_DEF. Without this an
# A/B arm silently compiles the default and both arms are one program -- which is
# how a knob gets "measured" at zero. Echoed so a run says what it built.
SM_EXTRA_DEF=${SM_EXTRA_DEF:-}
[ -n "$SM_EXTRA_DEF" ] && echo "  extra defines: $SM_EXTRA_DEF"

SRCS=$(make -pn 2>/dev/null | grep '^SM_C_SOURCES = ' | head -1 |
       sed 's/^SM_C_SOURCES = //; s|$(CORE_SM)|external/sm|g' | tr ' ' '\n' |
       grep '\.c$' | grep -v 'main_sm\.c$')

for f in $SRCS; do
  gcc -c -O1 -g -DNDEBUG -DTARGET_GNW -DHEADLESS -w $DEVICE_RULES $SM_EXTRA_DEF \
      -Iexternal/sm -Itools/sm_harness/shim "$f" -o "$OUT/$(basename "${f%.c}").o"
done
gcc -c -O1 -g -DTARGET_GNW -w $DEVICE_RULES $SM_EXTRA_DEF -Iexternal/sm -Itools/sm_harness/shim \
    tools/sm_harness/device_main.c -o "$OUT/main.o"
gcc -fsanitize=alignment -o "$OUT/sm_device" "$OUT"/*.o -lm
echo "  built: $OUT/sm_device   (device compile-time reality)"

ROM=$1
[ -z "$ROM" ] && exit 0

FRAMES=${2:-300}
LOG=$OUT/ubsan.log
UBSAN_OPTIONS=halt_on_error=0 "$OUT/sm_device" "$ROM" "$FRAMES" 2>"$LOG"

if grep -q "requires 8 byte alignment" "$LOG"; then
  echo "  FAIL: 64-bit access on an unaligned address — the device traps this (LDRD/STRD):"
  grep "requires 8 byte alignment" "$LOG" | sort -u | sed 's/^/    /'
  exit 1
fi
echo "  ok: $FRAMES frames, no 64-bit misaligned access (the class the device traps)"
