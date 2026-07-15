#!/bin/bash
# Generic SNES core on QEMU's Cortex-M7 (mps2-an500): executed-instruction
# counts per frame (emu vs audio), on a real ARMv7-M instruction stream.
#
#   bash tools/m7_qemu_rig/run_snes.sh <rom.smc> [frames]
#
# -icount shift=0: virtual time ticks 1 ns per executed instruction, the CMSDK
# timer runs on virtual time, so timer deltas are instruction counts (the rig
# calibrates the scale on itself at boot). Measures instructions/frame, A/B-able
# across code changes, comparable against a MHz budget. Does NOT model caches or
# wait states (QEMU has neither) — absolute device fps still comes from hardware.
# STATEHASH here matches tools/snes_harness for the same ROM + frame count.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes.sh <rom.smc> [frames]}"
FRAMES="${2:-1200}"

SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-O2 -g -ffunction-sections -fdata-sections"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

# ROM -> object (symbols _binary_rom_smc_start/end)
cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm rom.smc rom.o)

SRCS="$SM/src/snes/apu.c $SM/src/snes/cart.c $SM/src/snes/cpu.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime.c $RIG/rig_snes.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes.elf"

arm-none-eabi-size "$OUT/rig_snes.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes.elf"
