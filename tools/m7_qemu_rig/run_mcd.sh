#!/bin/bash
# Mega Drive (gwenesis) on QEMU's Cortex-M7 (mps2-an500): executed-instruction
# baseline for the Sega/Mega CD feasibility question.
#
#   bash tools/m7_qemu_rig/run_mcd.sh <rom.bin|md|gen> [frames]
#
# Same core sources the device links (single 68K + Z80 + YM2612 + SN76489 +
# VDP), run as a real ARMv7-M stream so timer deltas are instruction counts.
# Prints emu= instructions/frame per window + a RUNHASH. This is the DENOMINATOR
# the dual-68K Sega CD rig (rig_mcd.c) is measured against.
#
# What it does NOT measure: caches / wait states (QEMU has neither). Absolute
# device fps still comes from the device.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_mcd.sh <rom.bin> [frames]}"
FRAMES="${2:-3000}"

GW=external/gwenesis/src
RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=soft"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DTARGET_GNW -DIS_LITTLE_ENDIAN -DLSB_FIRST -DTABLES_FULL -DRIG_FRAMES=$FRAMES"
INC="-I$RIG/md_shim -I$GW/cpus/M68K -I$GW/cpus/Z80 -I$GW/sound -I$GW/bus -I$GW/vdp -I$GW/io \
     -I$GW/savestate -ICore/Inc -ICore/Inc/retro-go -ICore/Inc/porting -ICore/Src/porting/lib \
     -Iretro-go-stm32/components/odroid"

# gwenesis pair-swaps ROM_DATA in load_cartridge(); embed raw, the core swaps.
cp "$ROM" "$OUT/rom.md"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --redefine-sym _binary_rom_md_md_start=_binary_rom_md_start \
    --redefine-sym _binary_rom_md_md_end=_binary_rom_md_end \
    rom.md rom.o 2>/dev/null || arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm rom.md rom.o)

SRCS="$GW/cpus/M68K/m68kcpu.c $GW/cpus/Z80/Z80.c $GW/sound/z80inst.c \
      $GW/sound/ym2612.c $GW/sound/gwenesis_sn76489.c \
      $GW/bus/gwenesis_bus.c $GW/bus/gwenesis_sram.c $GW/bus/gwenesis_eeprom.c \
      $GW/io/gwenesis_io.c $GW/vdp/gwenesis_vdp_mem.c $GW/vdp/gwenesis_vdp_gfx.c \
      $RIG/rig_runtime.c $RIG/rig_mcd.c"

OBJS=""
for s in $SRCS; do
    o="$OUT/md_$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    OBJS="$OBJS $o"
done
OBJS="$OBJS $OUT/rom.o"

$CC $ARCH -T "$RIG/mps2_an500.ld" -nostartfiles -Wl,--gc-sections \
    -Wl,--defsym,__exidx_start=0 -Wl,--defsym,__exidx_end=0 \
    $OBJS -lm -o "$OUT/rig_mcd.elf"

arm-none-eabi-size "$OUT/rig_mcd.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_mcd.elf"
