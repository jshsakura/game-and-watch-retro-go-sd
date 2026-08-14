#!/bin/bash
# The generic SNES core on QEMU's Cortex-M7 running THE ENGINES THE DEVICE RUNS:
# the Thumb-2 65816 (SNES_THUMB2_CPU) and the Thumb-2 SPC700 (SPC_THUMB2_SPC),
# assembled from the same .S files the firmware links.
#
#   bash tools/m7_qemu_rig/run_snes_t2.sh <rom.smc> [frames]
#
# Why this exists and run_snes_hf.sh does not answer for it: that rig compiles
# cpu.c and spc.c as C. The device has not run either as C since 0725. A rig that
# runs a different interpreter measures a different program, and the CPI and
# "spin +38.8%" numbers taken from it in July were void for exactly that reason.
# Anything whose payoff is "opcodes not dispatched" MUST be measured here, where
# an opcode costs what it costs on the device.
#
# RIG_EXTRA_DEF passes defines through, which is how the A/B arms are built, e.g.
#   RIG_EXTRA_DEF=-DSNES_SPC_IDLE_SKIP=0 bash tools/m7_qemu_rig/run_snes_t2.sh rom.smc 700
#
# -icount shift=0: virtual time ticks 1 ns per executed instruction, so timer
# deltas are instruction counts. No caches, no wait states — absolute fps still
# comes from hardware; this gives an A/B-able instruction count.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_t2.sh <rom.smc> [frames]}"
# 1200, not 400. A Link to the Past spends its first ~500 frames on a black
# screen: the compositing loop runs on every line, but no background layer is
# enabled, so PpuDrawBackground_4bpp/2bpp are never entered. A 400-frame run
# therefore gates changes to the tile drawers by NOT EXECUTING THEM -- measured:
# bg_tile = 0 at 400 frames, 15.9 M at 1500. Anything touching the layer drawers
# needs a run long enough to reach lit pixels; check the `lit=` column, and if it
# is under a few thousand the renderer is not being tested.
FRAMES="${2:-1200}"

SM=external/sm
RIG=tools/m7_qemu_rig
OUT="${RIG_OUT:-$RIG/build_t2}"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES \
     -DRIG_WINDOW=${RIG_WINDOW:-200} -DSNES_THUMB2_CPU -DSPC_THUMB2_SPC ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$SM/src -I$RIG/shim -Itools/sm_harness/shim"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="$SM/src/snes/apu.c $SM/src/snes/cart.c $SM/src/snes/cpu.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/snes/dsp1_hle.c $SM/src/snes/rc_dispatch.c $SM/src/snes/spin_skip.c \
      $SM/src/snes/spin_bake.c $SM/src/tracing.c \
      $SM/src/snes/thumb2/cpu_thumb2_offsets_check.c \
      $SM/src/snes/thumb2/spc_thumb2_offsets_check.c \
      $RIG/rig_runtime_hf.c $RIG/rig_snes.c"

ASMS="$SM/src/snes/thumb2/snes_thumb2.S $SM/src/snes/thumb2/spc_thumb2.S"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
# The device assembles the engine with ASFLAGS, and ships SNES_ROMCACHE=0 --
# i.e. -DSNES_T2_NO_ROMCACHE, so the inline ROM page cache never serves and
# every opcode fetch calls snes_cpuRead. Without this the rig runs the engine
# with that cache ON and fetches never reach the bus at all, which is not the
# device's program: a bus census taken here read "90% of reads are WRAM",
# designed a reorder on it, and the reorder cost 1.7 drawn fps on hardware.
# RIG_ASM_DEF overrides it for anyone who wants the other configuration.
ASM_DEF="${RIG_ASM_DEF:--DSNES_T2_NO_ROMCACHE}"
for s in $ASMS; do
    o="$OUT/$(basename "${s%.S}").o"
    $CC -c $ARCH $OPT $DEF $ASM_DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes.elf"

arm-none-eabi-size "$OUT/rig_snes.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes.elf"
