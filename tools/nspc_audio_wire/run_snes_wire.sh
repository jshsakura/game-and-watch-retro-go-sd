#!/bin/bash
# Live-wired N-SPC HLE on the hard-float M7 rig (copy of run_snes_hf.sh with
# the wire sources). Requires build.sh to have generated /tmp/nspc_wire_build/
# {nspc_player_gen.c, apu_wire.c, rig_snes_wire.c}. WIRE_OFF=1 -> stock LLE
# reference from the same binary layout (wire disabled at runtime).
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_snes_wire.sh <rom.smc> [frames]}"
FRAMES="${2:-1500}"

SM=external/sm
RIG=tools/m7_qemu_rig
GEN=/tmp/nspc_wire_build
HERE=tools/nspc_audio_wire
OUT="$GEN/rig"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16"
OPT="-O2 -g -ffunction-sections -fdata-sections -ffp-contract=off"
DEF="-DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DRIG_FRAMES=$FRAMES -DRIG_WINDOW=${RIG_WINDOW:-200} ${RIG_EXTRA_DEF:-}"
INC="-I$SM -I$RIG/shim -Itools/sm_harness/shim"
SMINC="-iquote $SM/src"

cp "$ROM" "$OUT/rom.smc"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm --rename-section .data=.rom_blob,alloc,load,readonly,data,contents rom.smc rom.o)

SRCS="$SM/src/snes/cart.c $SM/src/snes/cpu.c $SM/src/snes/cx4_hle.c \
      $SM/src/snes/dma.c $SM/src/snes/dsp.c \
      $SM/src/snes/input.c $SM/src/snes/ppu.c $SM/src/snes/snes.c \
      $SM/src/snes/snes_other.c $SM/src/snes/spc.c $SM/src/tracing.c \
      $RIG/rig_runtime_hf.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC -w "$s" -o "$o"
    OBJS="$OBJS $o"
done
$CC -c $ARCH $OPT $DEF $INC -iquote $SM/src/snes -w "$GEN/apu_wire.c" -o "$OUT/apu_wire.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -include tools/nspc_hle/nspc_config.h -w "$GEN/nspc_player_gen.c" -o "$OUT/nspc_player_gen.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -Itools/nspc_hle -w tools/nspc_hle/nspc_variant.c -o "$OUT/nspc_variant.o"
$CC -c $ARCH $OPT $DEF $INC $SMINC -Itools/nspc_hle -I"$HERE" -Itools/snes_survey -w "$HERE/wire.c" -o "$OUT/wire.o"
$CC -c $ARCH $OPT $DEF $INC -w "$GEN/rig_snes_wire.c" -o "$OUT/rig_snes_wire.o"
OBJS="$OBJS $OUT/apu_wire.o $OUT/nspc_player_gen.o $OUT/nspc_variant.o $OUT/wire.o $OUT/rig_snes_wire.o $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500_snes.ld" -nostartfiles -Wl,--gc-sections \
    $OBJS -lm -o "$OUT/rig_snes_wire.elf"

arm-none-eabi-size "$OUT/rig_snes_wire.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_snes_wire.elf"
