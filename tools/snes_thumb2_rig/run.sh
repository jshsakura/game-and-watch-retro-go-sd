#!/bin/bash
# Differential harness: links REAL cpu.c (oracle + Stage-1 dispatcher) against
# REAL snes_thumb2.S, runs under QEMU mps2-an500 (Cortex-M7). Verifies the
# Thumb-2 fast path matches the C oracle byte-for-byte across all 256 opcodes
# and M/X/E flag combinations.
set -euo pipefail
cd "$(dirname "$0")/../.."

SM=external/sm
RIG=tools/snes_thumb2_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-O2 -g -ffunction-sections -fdata-sections"
DEF="-DNDEBUG -DSNES_THUMB2_CPU -DSNES_SPIN_SKIP -DTARGET_GNW"
INC="-I$SM/src/snes -I$SM/src -I$SM/src/snes/thumb2"

echo "[1/5] Assemble snes_thumb2.S ..."
$CC -c $ARCH $OPT $DEF $INC "$SM/src/snes/thumb2/snes_thumb2.S" -o "$OUT/snes_thumb2.o"

echo "[2/5] Compile cpu.c (oracle + dispatcher, production spin hooks) ..."
$CC -c $ARCH $OPT $DEF $INC -w "$SM/src/snes/cpu.c" -o "$OUT/cpu.o"

echo "[3/5] Compile real spin_skip.c ..."
$CC -c $ARCH $OPT $DEF $INC -w "$SM/src/snes/spin_skip.c" -o "$OUT/spin_skip.o"

echo "[4/5] Compile rig ..."
$CC -c $ARCH $OPT $DEF $INC "$RIG/rig_runtime.c" -o "$OUT/rig_runtime.o"
$CC -c $ARCH $OPT $DEF $INC "$RIG/rig_thumb2_diff.c" -o "$OUT/rig_thumb2_diff.o"

echo "[5/5] Link ..."
$CC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    $OUT/rig_runtime.o $OUT/rig_thumb2_diff.o $OUT/cpu.o $OUT/spin_skip.o \
    $OUT/snes_thumb2.o \
    -lm -o "$OUT/rig_thumb2.elf"

arm-none-eabi-size "$OUT/rig_thumb2.elf"

echo "[run] qemu-system-arm mps2-an500 ..."
timeout 300 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_thumb2.elf"
