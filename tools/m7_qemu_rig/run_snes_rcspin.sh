#!/bin/bash
# Spin-skip + 65816->C translator hybrid on the hard-float M7 rig.
#   bash tools/m7_qemu_rig/run_snes_rcspin.sh <rom.smc> [frames]
# PREREQ: tools/sfc_recomp/build.sh run for THE SAME ROM (fills /tmp/sfc_recomp_build).
# The hooked cpu_copy.c is placed in $OUT; rc_core_rig.c is generated into $OUT,
# so its quote-#include "cpu_copy.c" resolves to the hooked copy first.
# Gate: STATEHASH must equal the stock run_snes_hf.sh for the same ROM/frames.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_rcspin.sh <rom.smc> [frames]}"
FRAMES="${2:-1200}"

SM=external/sm
RIG=tools/m7_qemu_rig
GEN=/tmp/sfc_recomp_build
OUT="$RIG/build_rcspin"
mkdir -p "$OUT"

[ -f "$GEN/rc_sites.inc" ] && [ -f "$GEN/cpu_copy.c" ] || {
  echo "run tools/sfc_recomp/build.sh for this ROM first" >&2; exit 1; }

# rc_core for the rig (same splice as run_snes_rc.sh)
awk '/---- hybrid dispatch/{exit} {print}' tools/sfc_recomp/rc_core.c > "$OUT/rc_core_rig.c"
cat "$RIG/rc_dispatch_rig.c" >> "$OUT/rc_core_rig.c"
grep -q "rc_banks" "$OUT/rc_core_rig.c" || { echo "tail splice failed" >&2; exit 1; }

# purity-hooked interpreter copy for the hybrid TU (covers native + fallback:
# both use cpu_copy.c's cpu_read/cpu_write; folded fetches bypass them, which is
# correct — fetch reads are excluded by the detector anyway)
sed -e 's#return snes_cpuRead((Snes\*) cpu->mem, adr);#{ extern void snes_spin_read(Cpu*, uint32_t); snes_spin_read(cpu, adr); } return snes_cpuRead((Snes*) cpu->mem, adr);#' \
    -e 's#snes_cpuWrite((Snes\*) cpu->mem, adr, val);#{ extern unsigned long long g_write_seq; g_write_seq++; } snes_cpuWrite((Snes*) cpu->mem, adr, val);#' \
    "$GEN/cpu_copy.c" > "$OUT/cpu_copy.c"
grep -q "snes_spin_read" "$OUT/cpu_copy.c" || { echo "sed miss: spin_read" >&2; exit 1; }
grep -q "g_write_seq"    "$OUT/cpu_copy.c" || { echo "sed miss: write_seq" >&2; exit 1; }

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRC_STATS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

# cpu.c NOT in this list: rc_core_rig.c carries the (hooked) interpreter copy +
# the hybrid cpu_runOpcode.
SRCS="$SM/src/snes/apu.c $SM/src/snes/cart.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c $SM/src/snes/input.c \
      $SM/src/snes/ppu.c $SM/src/snes/snes.c $SM/src/snes/snes_other.c \
      $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime_hf.c $RIG/rig_snes_spin.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
$CC -c $ARCH $OPT $DEF $INC -I"$GEN" -I$SM/src/snes -w "$OUT/rc_core_rig.c" -o "$OUT/rc_core.o"
OBJS="$OBJS $OUT/rc_core.o $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes_rc.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_rcspin.elf"

arm-none-eabi-size "$OUT/rig_snes_rcspin.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_rcspin.elf"
