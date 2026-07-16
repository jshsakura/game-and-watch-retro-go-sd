#!/bin/bash
# Sega 32X (picodrive interpreter) on QEMU Cortex-M7: host insn/frame.
#   bash tools/m7_qemu_rig/run_32x.sh <rom.32x> [frames]
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_32x.sh <rom.32x> [frames]}"
FRAMES="${2:-600}"

PD="${PD_DIR:-/tmp/claude-1001/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/64ada589-e1c0-4631-9393-e9fa961f0c74/scratchpad/picodrive}"
RIG=tools/m7_qemu_rig
OUT="$RIG/build/32x"
mkdir -p "$OUT"

CC=arm-none-eabi-gcc
ARCH="-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 -ffp-contract=off"
OPT="-O2 -g -fno-strict-aliasing -ffunction-sections -fdata-sections -fcommon"
DEF="-DEMU_F68K -D_USE_CZ80 -DDRC_SH2 -DNDEBUG -DRIG_FRAMES=$FRAMES ${EXTRA_DEF:-}"
INC="-I$PD -I$PD/pico -I$PD/cpu -I$PD/cpu/fame -I$PD/zlib"

# ROM -> .rom section, symbols _binary_rom_32x_start/end
cp "$ROM" "$OUT/rom.32x"
(cd "$OUT" && arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
    --rename-section .data=.rom,alloc,load,readonly,data,contents rom.32x rom.o)

# picodrive 32X core (interpreter; no DRC compiler.c, no CD, no libretro frontend)
SRCS="
cpu/sh2/sh2.c cpu/sh2/mame/sh2pico.c
cpu/fame/famec.c cpu/cz80/cz80.c cpu/drc/cmn.c
pico/32x/32x.c pico/32x/draw.c pico/32x/memory.c pico/32x/pwm.c pico/32x/sh2soc.c
pico/cart.c pico/memory.c pico/draw.c pico/draw2.c pico/sek.c pico/videoport.c
pico/media.c pico/pico.c pico/misc.c pico/patch.c pico/state.c pico/z80if.c
pico/eeprom.c pico/debug.c pico/mode4.c pico/sms.c pico/cd/gfx_dma.c
pico/carthw/carthw.c pico/carthw/eeprom_spi.c
pico/carthw/svp/memory.c pico/carthw/svp/ssp16.c pico/carthw/svp/svp.c
pico/pico/memory.c pico/pico/pico.c pico/pico/xpcm.c
pico/sound/sound.c pico/sound/mix.c pico/sound/sn76496.c pico/sound/ym2612.c
pico/sound/ym2413.c pico/sound/resampler.c
zlib/adler32.c zlib/compress.c zlib/crc32.c zlib/deflate.c zlib/gzio.c
zlib/infback.c zlib/inffast.c zlib/inflate.c zlib/inftrees.c zlib/trees.c
zlib/uncompr.c zlib/zutil.c
"

objs="$OUT/rom.o"
for s in $SRCS; do
    o="$OUT/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    $CC -c $ARCH $OPT $DEF $INC "$PD/$s" -o "$o"
    objs="$objs $o"
done
# rig harness + runtime
$CC -c $ARCH $OPT $DEF -I$PD "$RIG/rig_32x.c"        -o "$OUT/rig_32x.o"
$CC -c $ARCH $OPT $DEF "$RIG/rig_32x_stubs.c" -o "$OUT/rig_32x_stubs.o"
$CC -c $ARCH $OPT $DEF -I$PD -I$PD/pico -I$PD/cpu -I$PD/cpu/fame -I$PD/zlib "$RIG/rig_32x_globals.c" -o "$OUT/rig_32x_globals.o"
$CC -c $ARCH $OPT $DEF        "$RIG/rig_runtime.c" -o "$OUT/rig_runtime.o"
objs="$objs $OUT/rig_32x.o $OUT/rig_32x_stubs.o $OUT/rig_32x_globals.o $OUT/rig_runtime.o"

$CC $ARCH -T "$RIG/mps2_an500_32x.ld" -nostartfiles -Wl,--gc-sections \
    $objs -lm -lc -o "$OUT/rig_32x.elf"
arm-none-eabi-size "$OUT/rig_32x.elf"

timeout 1800 qemu-system-arm -machine mps2-an500 -nographic -semihosting \
    -icount shift=0,align=off,sleep=off -kernel "$OUT/rig_32x.elf" 2>&1
