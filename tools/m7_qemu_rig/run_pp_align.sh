#!/bin/bash
# gnw_line_pp() alignment gate on QEMU Cortex-M7. See rig_pp_align.c.
#   bash tools/m7_qemu_rig/run_pp_align.sh
# Exit 0 = PASS, 1 = FAIL or fault, 77 = toolchain/qemu missing (skipped, said so).
set -uo pipefail
cd "$(dirname "$0")/../.."

command -v qemu-system-arm >/dev/null || { echo "[pp_align] SKIP: qemu-system-arm not found"; exit 77; }
# Compile with the pinned builder when the host has no arm-none-eabi-gcc: the same
# compiler the firmware is built with, which is the one that fuses the loads.
BUILDER="${RIG_BUILDER:-sylverb/retro-go-sd-builder:v1.5}"
if command -v arm-none-eabi-gcc >/dev/null; then
  GCC() { arm-none-eabi-gcc "$@"; }
elif command -v docker >/dev/null && docker image inspect "$BUILDER" >/dev/null 2>&1; then
  GCC() { docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/work" -w /work "$BUILDER" arm-none-eabi-gcc "$@"; }
else
  echo "[pp_align] SKIP: no arm-none-eabi-gcc and no $BUILDER image"; exit 77
fi

PD="${PD_DIR:-external/picodrive}"
RIG=tools/m7_qemu_rig
OUT="${RIG_OUT:-$RIG/build/pp_align}"
mkdir -p "$OUT"

ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections -fcommon"
DEF="-DGNW_32X_CORE -DEMU_G68K -DTABLES_FULL -D_USE_CZ80 -DNDEBUG"
INC="-I$PD -I$PD/pico -I$PD/cpu -I$PD/zlib"

GCC $ARCH $OPT $DEF $INC -c "$RIG/rig_pp_align.c" -o "$OUT/t.o" || exit 1
GCC $ARCH $OPT $DEF -c "$RIG/rig_runtime.c" -o "$OUT/rt.o" || exit 1
GCC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    "$OUT/t.o" "$OUT/rt.o" -lm -lc -o "$OUT/pp_align.elf" 2> "$OUT/link.log" \
  || { echo "[pp_align] link failed"; tail -5 "$OUT/link.log"; exit 1; }

log=$(timeout "${RIG_TIMEOUT:-60}" qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/pp_align.elf" 2>&1)
echo "$log"
echo "$log" | grep -q '^PASS$' && exit 0
echo "[pp_align] no PASS: mismatch, or the CPU faulted and never reached the end"
exit 1
