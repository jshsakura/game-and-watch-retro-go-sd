#!/bin/bash
# Virtual Boy on QEMU's Cortex-M7 (mps2-an500): executed-instruction counts
# per frame, on a real ARMv7-M instruction stream.
#
#   bash tools/m7_qemu_rig/run_vb.sh <rom.vb> [frames]
#
# -icount shift=0 makes virtual time tick 1 ns per executed instruction; the
# board's CMSDK timer runs on virtual time, so timer deltas are instruction
# counts (the rig calibrates the exact scale on itself at boot and prints it).
#
# What this measures: instructions per frame (emulation vs blit), A/B-able
# across code changes, comparable against a MHz budget. What it does NOT
# measure: caches and wait states (QEMU has neither) — absolute device fps
# still comes from the device. Frame hashes printed here match linux/vb's for
# the same ROM + frame count (same core, same input script).
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_vb.sh <rom.vb> [frames]}"
FRAMES="${2:-3000}"

RV=external/red-viper
RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
CXX=arm-none-eabi-g++
# Hard float — MUST match the device (Makefile.common: -mfloat-abi=hard
# -mfpu=fpv5-d16). The V810 has an FPU and 3D games lean on it; a soft-float
# rig counts each float op as a multi-instruction libgcc call and overstates
# the emulation cost the device pays in one VFP instruction. -ffp-contract=off
# keeps results reproducible (no surprise FMA fusion).
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DGNW_VB_DEVICE -DDEBUGLEVEL=0 -DVB_LEFT_EYE_ONLY -DRIG_FRAMES=$FRAMES"
INC="-I$RV/source/common -I$RV/include -I$RV/source/common/inih"

# ROM -> object (symbols _binary_rom_vb_start/end)
cp "$ROM" "$OUT/rom.vb"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm rom.vb rom.o)

SRCS_C="$RV/source/common/v810_cpu.c $RV/source/common/v810_ins.c \
        $RV/source/common/v810_mem.c $RV/source/common/interpreter.c \
        $RV/source/common/vb_set.c $RV/source/common/rom_db.c \
        $RV/source/common/patches.c $RV/source/common/video_common.c \
        $RV/source/common/inih/ini.c \
        $RIG/rig_runtime.c $RIG/rig_vb.c"

OBJS=""
for s in $SRCS_C; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    OBJS="$OBJS $o"
done
$CXX -c $ARCH $OPT $DEF $INC -fno-rtti -fno-exceptions \
    "$RV/source/common/video_soft.cpp" -o "$OUT/video_soft.o"
OBJS="$OBJS $OUT/video_soft.o $OUT/rom.o"

$CXX $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_vb.elf"

arm-none-eabi-size "$OUT/rig_vb.elf"

# align=off: don't slave execution speed to wall clock; sleep=off: don't idle.
timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_vb.elf"
