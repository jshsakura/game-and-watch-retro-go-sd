#!/bin/bash
# Build reusable SNES cost-center ELFs.  ROMs are injected at run time with
# QEMU's loader, so this compile/link happens once for an entire ROM library.
set -euo pipefail
cd "$(dirname "$0")/../.."

FRAMES="${1:-300}"
SM=external/sm
RIG=tools/m7_qemu_rig
OUT="$RIG/build_cost"
mkdir -p "$OUT"

python3 - "$SM/src/snes/apu.c" "$OUT/apu_cost.c" <<'PY'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
assert src.count("spc_runOpcode(apu->spc)") == 2
assert src.count("dsp_cycle(apu->dsp);") == 2
src = src.replace(
    "spc_runOpcode(apu->spc)",
    "({ uint32_t _t=rig_timer_now(); int _r=spc_runOpcode(apu->spc); "
    "g_spc_ticks += (uint32_t)(rig_timer_now()-_t); _r; })")
src = src.replace(
    "dsp_cycle(apu->dsp);",
    "{ uint32_t _t=rig_timer_now(); dsp_cycle(apu->dsp); "
    "g_dsp_ticks += (uint32_t)(rig_timer_now()-_t); g_dsp_calls++; }")
open(sys.argv[2], "w", encoding="utf-8").write(src)
PY

cat > "$OUT/cost_defs.h" <<'EOF'
#ifndef COST_DEFS_H
#define COST_DEFS_H
#include <stdint.h>
extern uint64_t g_spc_ticks, g_dsp_ticks, g_dsp_calls;
uint32_t rig_timer_now(void);
#endif
EOF

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
BASE_DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_ROM_LOADER -DRIG_COST_PROF -DRIG_DEVICE_VIDEO -DRIG_INPUT_TAP -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-100}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"
SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c $SM/src/snes/dma.c \
      $SM/src/snes/dsp.c $SM/src/snes/input.c $SM/src/snes/ppu.c \
      $SM/src/snes/snes.c $SM/src/snes/snes_other.c $SM/src/snes/spc.c \
      $SM/src/snes/rc_dispatch.c $SM/src/tracing.c $RIG/rig_runtime_hf.c \
      $RIG/rig_snes.c"

build_one() {
  local name=$1 extra=$2 dir="$OUT/$1"
  mkdir -p "$dir"
  local defs="$BASE_DEF $extra" objs=""
  $CC -c $ARCH $OPT $defs $INC -iquote "$SM/src/snes" -iquote "$SM/src" \
      -include "$OUT/cost_defs.h" -w "$OUT/apu_cost.c" -o "$dir/apu_cost.o"
  objs="$dir/apu_cost.o"
  for src in $SRCS; do
    local obj="$dir/$(basename "${src%.c}").o"
    $CC -c $ARCH $OPT $defs $INC -w "$src" -o "$obj"
    objs="$objs $obj"
  done
  $CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
      $objs -lm -o "$OUT/snes_cost_$name.elf"
}

build_one on ""
build_one off "-DRIG_FRAMESKIP"
arm-none-eabi-size "$OUT/snes_cost_on.elf" "$OUT/snes_cost_off.elf"
printf '%s\n' "$FRAMES" > "$OUT/frames.txt"
printf 'SNES cost ELFs ready: %s frames per ROM\n' "$FRAMES"
