#!/bin/bash
# Base-cost breakdown build of the SNES M7 rig, HARD-FLOAT (device ABI):
#   cpu   = executed insns inside cpu_runOpcode (65816 interpreter incl. its bus reads)
#   dsp   = executed insns inside dsp_cycle (S-DSP mixer; a dedicated block mixer's cap)
# bracketed with the rig clock; everything else in `emu` is event-loop bookkeeping,
# DMA, PPU (if rendering) and the apuCatchup FMA accumulation.
# external/ is never edited -- generated copies only.
#
#   bash tools/m7_qemu_rig/run_snes_break.sh <rom.smc> [frames]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_break.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"

SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build_break"
mkdir -p "$OUT"

cat > "$OUT/break_defs.h" <<'EOF'
#ifndef BREAK_DEFS_H
#define BREAK_DEFS_H
#include <stdint.h>
extern uint64_t g_cpu_ticks, g_dsp_ticks;
uint32_t rig_timer_now(void);
#endif
EOF

# instrumented apu.c: bracket both dsp_cycle call sites
sed 's#dsp_cycle(apu->dsp);#{ uint32_t _dt=rig_timer_now(); dsp_cycle(apu->dsp); g_dsp_ticks += (uint32_t)(rig_timer_now()-_dt); }#g' \
    "$SM/src/snes/apu.c" > "$OUT/apu_break.c"
grep -c 'g_dsp_ticks +=' "$OUT/apu_break.c" | xargs echo "  instrumented dsp_cycle sites:"

# instrumented rig: bracket both cpu_runOpcode call sites, add globals + window/final prints
python3 - "$RIG/rig_snes.c" "$OUT/rig_snes_break.c" <<'PY'
import sys
src = open(sys.argv[1]).read()
n = src.count("int cycles = cpu_runOpcode(snes->cpu);")
assert n == 2, n
src = src.replace(
    "int cycles = cpu_runOpcode(snes->cpu);",
    "uint32_t _ct = rig_timer_now(); int cycles = cpu_runOpcode(snes->cpu); g_cpu_ticks += (uint32_t)(rig_timer_now()-_ct);")
src = src.replace(
    "static Snes *g_the_snes;",
    "static Snes *g_the_snes;\nuint64_t g_cpu_ticks, g_dsp_ticks;\nstatic uint64_t g_last_cpu, g_last_dsp;")
old_w = '''      printf("w%05d emu=%lu apu=%lu insn/frame fb=%08lx lit=%d\\n",
             frame + 1, (unsigned long)emu_i, (unsigned long)apu_i,
             (unsigned long)(uint32_t)h, lit);'''
new_w = '''      uint64_t cpu_i = (g_cpu_ticks - g_last_cpu) * ipt_x1000 / 1000 / RIG_WINDOW;
      uint64_t dsp_i = (g_dsp_ticks - g_last_dsp) * ipt_x1000 / 1000 / RIG_WINDOW;
      g_last_cpu = g_cpu_ticks; g_last_dsp = g_dsp_ticks;
      printf("w%05d emu=%lu apu=%lu cpu=%lu dsp=%lu insn/frame fb=%08lx lit=%d\\n",
             frame + 1, (unsigned long)emu_i, (unsigned long)apu_i,
             (unsigned long)cpu_i, (unsigned long)dsp_i,
             (unsigned long)(uint32_t)h, lit);'''
assert old_w in src
src = src.replace(old_w, new_w)
old_f = '''  printf("[snes-qemu] done %d frames STATEHASH=%08lx avg emu=%lu apu=%lu insn/frame\\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));'''
new_f = '''  printf("[snes-qemu] done %d frames STATEHASH=%08lx avg emu=%lu apu=%lu cpu=%lu dsp=%lu insn/frame\\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_cpu_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_ticks * ipt_x1000 / 1000 / frames));'''
assert old_f in src
src = src.replace(old_f, new_f)
open(sys.argv[2], "w").write(src)
print("  rig_snes_break.c written")
PY

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime_hf.c"

OBJS=""
$CC -c $ARCH $OPT $DEF $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
    -include "$OUT/break_defs.h" -w "$OUT/apu_break.c" -o "$OUT/apu_break.o"
OBJS="$OBJS $OUT/apu_break.o"
$CC -c $ARCH $OPT $DEF $INC -include "$OUT/break_defs.h" -w "$OUT/rig_snes_break.c" -o "$OUT/rig_snes_break.o"
OBJS="$OBJS $OUT/rig_snes_break.o"
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_break.elf"

arm-none-eabi-size "$OUT/rig_snes_break.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_break.elf"
