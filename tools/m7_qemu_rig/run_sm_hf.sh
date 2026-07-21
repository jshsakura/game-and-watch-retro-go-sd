#!/bin/bash
# Super Metroid NATIVE PORT on QEMU's Cortex-M7 (mps2-an500), hard-float (device
# ABI). Instruction counts per frame for SM's hand-decompiled port, so today's
# shared-ppu.c optimizations show up in SM's own ledger.
#
#   [PPU_REV=<ppu.c path>] RIG_EXTRA_DEF="-DRIG_INPUT_TAP" bash tools/m7_qemu_rig/run_sm_hf.sh <sm.sfc> [frames]
#
# PPU_REV lets an A/B swap in a different ppu.c (e.g. the pre-optimization one via
# `git show 31ce2e6^:src/snes/ppu.c > /tmp/ppu_old.c`) without touching the tree.
# insn != cycle; QEMU models no caches — the device's frame ledger is the judge.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_sm_hf.sh <sm.sfc> [frames]}"
FRAMES="${2:-1200}"

SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build_sm"
mkdir -p "$OUT"; rm -f "$OUT"/*.o

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
# SM device reality: TARGET_GNW (apu NULL, spc_player is the sound chip). NOT
# GNW_SNES_CORE — that is the generic interpreter core, a different program.
DEF="-DNDEBUG -DTARGET_GNW -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

# The device's SM source set (Makefile's SM_C_SOURCES, main_sm.c excluded — the
# rig main stands where the firmware glue does).
SRCS=$(make -pn 2>/dev/null | grep '^SM_C_SOURCES = ' | head -1 |
       sed 's/^SM_C_SOURCES = //; s|$(CORE_SM)|external/sm|g' | tr ' ' '\n' |
       grep '\.c$' | grep -v 'main_sm\.c$')

# Optional A/B: substitute a different ppu.c (compiled under a temp name to a
# ppu.o so it takes the real one's slot).
PPU_REV="${PPU_REV:-}"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    if [ -n "$PPU_REV" ] && [ "$(basename "$s")" = "ppu.c" ]; then
        # a ppu.c pulled out of git history includes "ppu.h" relative to its own
        # dir — give it the real snes/ dir so those same-dir includes resolve.
        $CC -c $ARCH $OPT $DEF $INC -Iexternal/sm/src/snes -w "$PPU_REV" -o "$o"
    else
        $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    fi
    OBJS="$OBJS $o"
done
$CC -c $ARCH $OPT $DEF $INC -w "$RIG/rig_runtime_hf.c" -o "$OUT/rig_runtime_hf.o"
$CC -c $ARCH $OPT $DEF $INC -w "$RIG/rig_sm.c" -o "$OUT/rig_sm.o"
OBJS="$OBJS $OUT/rig_runtime_hf.o $OUT/rig_sm.o"

# ROM -> PSRAM blob (symbols _binary_rom_smc_start/end)
cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_sm.elf"
arm-none-eabi-size "$OUT/rig_sm.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_sm.elf"
