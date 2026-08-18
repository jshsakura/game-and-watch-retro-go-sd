#!/usr/bin/env bash
# The 32X core, natively, at host speed.
#
#   run.sh <rom.32x> [frames] [pad-pattern]      FB_OUT=x.raw to dump the screen
#
# Why this exists. tools/m7_qemu_rig answers "how many instructions does a frame
# cost on the device's own ISA", and it answers it slowly: ~5 minutes for 500
# frames under icount. That is fine for an A/B and useless for a search. Every
# 32X guest profile this repo had ever taken was of the attract demo, because
# nobody could afford to hunt for an input sequence that reaches a level five
# minutes at a time -- and a pad script that stopped at frame 560 stopped 60
# frames short of Doom's first level.
#
# This builds the SAME picodrive sources with the SAME device defines
# (-DGNW_32X_CORE) for the host, and runs 900 frames in about a second. Use it
# to find the sequence, then replay it in the rig where the instruction counts
# mean something.
#
# What it is NOT: a performance instrument. Host instructions are not device
# instructions; nothing here may be quoted as a cost.
#
# Three things that cost an afternoon and are now handled for you:
#   * the ROM must be the byte order the card holds. Feed it the raw .32x; a
#     byteswapped copy loads, reports the right size, and then never writes ADEN,
#     so 32X startup never fires and every frame is black.
#   * emu_32x_startup() must re-apply PicoDrawSetOutFormat + PicoDrawSetOutBuf.
#     They only route to their 32X variants once PAHW_32X is set.
#   * gnw_m68k_bank_alloc() takes NO argument. Declaring one allocates a
#     garbage-sized buffer and the run dies at exit with "malloc(): corrupted
#     top size".
set -euo pipefail
cd "$(dirname "$0")/../.."
ROM="${1:?usage: run.sh <rom.32x> [frames] [pad-pattern]}"
FRAMES="${2:-900}"
PAT="${3:-amash}"
PD=external/picodrive
OUT="${HOST_OUT:-tools/pico_host/build}"
mkdir -p "$OUT"

DEF="-DEMU_G68K -DTABLES_FULL -D_USE_CZ80 -DNDEBUG -DGNW_32X_CORE"
INC="-I$PD -I$PD/pico -I$PD/cpu -I$PD/cpu/fame -I$PD/zlib"
SRCS="cpu/sh2/sh2.c cpu/sh2/mame/sh2pico.c cpu/sh2/mame/sh2dasm.c
      cpu/gwenesis68k/m68kcpu.c cpu/gwenesis68k/g68k_bus.c cpu/cz80/cz80.c
      pico/32x/32x.c pico/32x/draw.c pico/32x/memory.c pico/32x/pwm.c pico/32x/sh2soc.c
      pico/cart.c pico/memory.c pico/draw.c pico/sek.c pico/videoport.c
      pico/media.c pico/pico.c pico/misc.c pico/patch.c pico/z80if.c
      pico/eeprom.c pico/state.c pico/carthw/carthw.c pico/carthw/eeprom_spi.c
      pico/sound/sound.c pico/sound/mix.c pico/sound/sn76496.c pico/sound/ym2612.c
      pico/sound/resampler.c zlib/crc32.c"

objs=""
for s in $SRCS; do
  o="$OUT/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
  gcc -O1 -w -fcommon $DEF $INC -c "$PD/$s" -o "$o"
  objs="$objs $o"
done
for s in host_drv host_stubs host_draw2fb; do
  gcc -O1 -w -fcommon $DEF $INC -c "tools/pico_host/$s.c" -o "$OUT/$s.o"
  objs="$objs $OUT/$s.o"
done
gcc -o "$OUT/host_drv" $objs -lm
exec "$OUT/host_drv" "$ROM" "$FRAMES" "$PAT"
