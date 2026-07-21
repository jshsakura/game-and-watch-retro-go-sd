#!/bin/bash
# Hybrid (65816->C static translator) SNES core on the hard-float M7 rig.
#
#   bash tools/m7_qemu_rig/run_snes_rc.sh <rom.smc> [frames]
#
# PREREQ: tools/sfc_recomp/build.sh must have been run for THE SAME ROM first —
# it generates /tmp/sfc_recomp_build/{cpu_copy.c, rc_sites.inc} (per-ROM) that
# this build includes. The dispatch tail is replaced with the bank-table variant
# (rc_dispatch_rig.c): the host's flat 16M-entry map is 32 MB and the board has
# 16 MB. Everything else matches run_snes_hf.sh so STATEHASH is comparable.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_rc.sh <rom.smc> [frames]}"
FRAMES="${2:-1200}"

SM=external/sm
RIG=tools/m7_qemu_rig
GEN="${SFC_RECOMP_OUT:-/tmp/sfc_recomp_build}"
OUT="$GEN/rig"
mkdir -p "$OUT"

[ -f "$GEN/rc_sites.inc" ] && [ -f "$GEN/cpu_copy.c" ] || {
  echo "run tools/sfc_recomp/build.sh for this ROM first" >&2; exit 1; }

# rc_core for the rig: same file as the host PoC up to the dispatch marker,
# then the bank-table dispatch. Byte-identical folded helpers + generated sites.
awk '/---- hybrid dispatch/{exit} {print}' tools/sfc_recomp/rc_core.c > "$OUT/rc_core_rig.c"
cat "$RIG/rc_dispatch_rig.c" >> "$OUT/rc_core_rig.c"
grep -q "rc_banks" "$OUT/rc_core_rig.c" || { echo "tail splice failed" >&2; exit 1; }

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

# cpu.c is NOT in this list: rc_core_rig.c carries the interpreter copy + the
# hybrid cpu_runOpcode.
SRCS="$SM/src/snes/apu.c $SM/src/snes/cart.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime_hf.c $RIG/rig_snes_rc.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
$CC -c $ARCH $OPT $DEF $INC -I"$GEN" -I$SM/src/snes -w "$OUT/rc_core_rig.c" -o "$OUT/rc_core.o"
OBJS="$OBJS $OUT/rc_core.o $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes_rc.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_rc.elf"

arm-none-eabi-size "$OUT/rig_snes_rc.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_rc.elf"
