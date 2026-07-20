#!/bin/bash
# CPS-1 on QEMU's Cortex-M7 (mps2-an500): executed-instruction counts per
# frame, on a real ARMv7-M instruction stream. Skeleton stage (no real
# 68000/Z80/PPU/sound core yet) -- see docs/CPS1_FEASIBILITY.md and
# tools/m7_qemu_rig/rig_cps1.c.
#
#   bash tools/m7_qemu_rig/run_cps1.sh [frames]
#
# rig_runtime.c and mps2_an500.ld are reused verbatim from rig_vb.c -- see
# docs/HARNESSES.md ("core-agnostic: copy rig_vb.c's shape").
set -euo pipefail
cd "$(dirname "$0")/../.."

FRAMES="${1:-600}"

RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
# Hard float -- MUST match the device (Makefile.common: -mfloat-abi=hard
# -mfpu=fpv5-d16). The stub is integer-only today, but a real 68000 core
# stays on the device's float ABI from day one this way.
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DRIG_FRAMES=$FRAMES"
INC="-ICore/Src/porting/cps1 -I$RIG"

SRCS_C="Core/Src/porting/cps1/cps1_core.c Core/Src/porting/cps1/cps1_cpu68k.c \
        Core/Src/porting/cps1/cps1_rc_runtime.c Core/Src/porting/cps1/cps1_rc_generated.c \
        Core/Src/porting/cps1/cps1_rom.c Core/Src/porting/cps1/cps1_ppu.c \
        Core/Src/porting/cps1/cps1_bg.c Core/Src/porting/cps1/cps1_sound_hle.c \
        $RIG/rig_runtime.c $RIG/rig_cps1.c"

OBJS=""
for s in $SRCS_C; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    OBJS="$OBJS $o"
done

$CC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_cps1.elf"

arm-none-eabi-size "$OUT/rig_cps1.elf"

# align=off: don't slave execution speed to wall clock; sleep=off: don't idle.
timeout 600 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_cps1.elf"
