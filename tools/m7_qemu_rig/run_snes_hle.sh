#!/bin/bash
# HLE-measurement build of the SNES M7 rig: apu.c is sed-instrumented into a
# generated apu_hle.c that accumulates the executed-instruction time spent inside
# spc_runOpcode (the SPC700 CPU interpreter) into g_spc_ticks. rig_snes_hle.c
# prints a third `spc700` figure and derives baseline vs HLE-ceiling fps.
# The submodule (external/) is NEVER edited -- only a generated copy.
#
#   bash tools/m7_qemu_rig/run_snes_hle.sh <rom.smc> [frames]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_hle.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"

SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

# --- generated, instrumented apu.c (submodule untouched) ---
cat > "$OUT/hle_defs.h" <<'EOF'
#ifndef HLE_DEFS_H
#define HLE_DEFS_H
#include <stdint.h>
extern uint64_t g_spc_ticks;
uint32_t rig_timer_now(void);
#endif
EOF
# wrap BOTH `apu->cpuCyclesLeft = spc_runOpcode(apu->spc);` sites with rig-clock timing
sed 's#apu->cpuCyclesLeft = spc_runOpcode(apu->spc);#{ uint32_t _st=rig_timer_now(); apu->cpuCyclesLeft = spc_runOpcode(apu->spc); g_spc_ticks += (uint32_t)(rig_timer_now()-_st); }#g' \
    "$SM/src/snes/apu.c" > "$OUT/apu_hle.c"
grep -c 'g_spc_ticks +=' "$OUT/apu_hle.c" | xargs echo "  instrumented spc_runOpcode sites:"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-O2 -g -ffunction-sections -fdata-sections"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

# apu_hle.c replaces apu.c; rig_snes_hle.c replaces rig_snes.c
SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime.c $RIG/rig_snes_hle.c"

OBJS=""
# instrumented apu with the force-included defs header
# apu_hle.c lives in $OUT, so its quote-includes ("apu.h", ...) need the original
# dir on the -iquote path. -iquote (not -I) so external/sm/src/features.h does not
# shadow glibc <features.h> in the angle-bracket chain.
$CC -c $ARCH $OPT $DEF $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
    -include "$OUT/hle_defs.h" -w "$OUT/apu_hle.c" -o "$OUT/apu_hle.o"
OBJS="$OBJS $OUT/apu_hle.o"
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_hle.elf"

arm-none-eabi-size "$OUT/rig_snes_hle.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_hle.elf"
