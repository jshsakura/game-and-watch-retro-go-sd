#!/bin/bash
# What does ONE REAL Tenchi wo Kurau II frame cost the renderer, in ARM
# instructions, on this device's own ISA? See tools/m7_qemu_rig/rig_cps1_frame.c
# for what is being measured and why it is a FLOOR, not a result.
#
#   bash tools/m7_qemu_rig/run_cps1_frame.sh [frames]
#
# This is the rig that established "graphics is 75.3% of the frame and the
# blitter is where optimisation belongs" (commit c5c85161). That commit shipped
# the rig's .c and .ld but not this runner, so the number could not be
# reproduced or A/B'd -- which is the whole point of having it.
#
# Inputs are three blobs captured from the game's own state, NOT synthetic:
#   gfxram.bin   (0x30000 B)  the game's gfxram after 3600 booted frames
#   cpsregs.bin  (0xC0 words) the CPS-A registers at that same moment
#   gfxrom.bin   (4 MB)       the real GFX ROM
# Regenerate them by running the Linux harness against a real ROM:
#   cd linux && make -f Makefile.cps1 && ./cps1 --real-frame <romdir>
# (real_frame_harness.c writes all three into $CPS1_DUMPS.)
set -euo pipefail
cd "$(dirname "$0")/../.."

FRAMES="${1:-20}"
DUMPS="${CPS1_DUMPS:-/tmp/cps1_rom}"

RIG=tools/m7_qemu_rig
OUT="$RIG/build"
mkdir -p "$OUT"

for b in gfxram.bin cpsregs.bin gfxrom.bin; do
    if [ ! -f "$DUMPS/$b" ]; then
        echo "MISSING $DUMPS/$b -- this rig measures real captured state, not a synthetic scene."
        echo "Regenerate the dumps (see the header of this file), or point CPS1_DUMPS at them."
        exit 1
    fi
done

CC=arm-none-eabi-gcc
OBJCOPY=arm-none-eabi-objcopy
# Hard float -- MUST match the device (Makefile.common: -mfloat-abi=hard
# -mfpu=fpv5-d16), same rule as run_cps1.sh.
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections"
DEF="-DRIG_FRAMES=$FRAMES"
INC="-ICore/Src/porting/cps1 -I$RIG"

SRCS_C="Core/Src/porting/cps1/cps1_rom.c Core/Src/porting/cps1/cps1_ppu.c \
        Core/Src/porting/cps1/cps1_bg.c \
        $RIG/rig_runtime.c $RIG/rig_cps1_frame.c"

OBJS=""
for s in $SRCS_C; do
    o="$OUT/$(basename "${s%.c}").o"
    $CC -c $ARCH $OPT $DEF $INC "$s" -o "$o"
    OBJS="$OBJS $o"
done

# objcopy derives _binary_<name>_bin_start from the INPUT PATH, so each blob has
# to be converted from inside its own directory or the symbol names gain the
# path. The 4 MB GFX ROM is renamed into .bigrom: CODE is 4 MB total and already
# holds text+rodata, so mps2_an500_bigrom.ld parks the blob in PSRAM instead.
ABS_OUT="$PWD/$OUT"
( cd "$DUMPS" \
  && $OBJCOPY -I binary -O elf32-littlearm -B arm \
        --rename-section .data=.bigrom,alloc,load,readonly,data,contents \
        gfxrom.bin "$ABS_OUT/gfxrom.o" \
  && $OBJCOPY -I binary -O elf32-littlearm -B arm gfxram.bin  "$ABS_OUT/gfxram.o" \
  && $OBJCOPY -I binary -O elf32-littlearm -B arm cpsregs.bin "$ABS_OUT/cpsregs.o" )
OBJS="$OBJS $OUT/gfxrom.o $OUT/gfxram.o $OUT/cpsregs.o"

# No --gc-sections here: the blobs are referenced only by linker-generated
# _binary_* symbols, and gc-sections would drop the ROM out from under the rig.
$CC $ARCH -T "$RIG/mps2_an500_bigrom.ld" -nostartfiles \
    $OBJS -lm -o "$OUT/rig_cps1_frame.elf"

arm-none-eabi-size "$OUT/rig_cps1_frame.elf"

# align=off: don't slave execution speed to wall clock; sleep=off: don't idle.
timeout 900 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_cps1_frame.elf"
