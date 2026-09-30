#!/usr/bin/env bash
# The Sega CD core, natively, at host speed.
#
#   run.sh <game.cue> [frames] [pad-pattern]
#          FB_OUT=x.raw   dump the final screen (1 byte/pixel, 320x240)
#          STATE_IN=x.sgm replay a device savestate (skips the 8-byte SCDP header)
#          BIOS_DIR=dir   BIOS lookup (default /media/pi/EXTERNAL/BIOS)
#
# Same idea as tools/pico_host (the 32X harness): the library is CHD-only and
# CHD cannot be decoded on-device (libchdr wants ~150-200KB the core does not
# have), so every game arrives as cue/bin via tools/segacd_chd_convert.sh --
# and this is where a converted disc proves it boots before anyone walks to
# the device. It builds the SAME picodrive sources the firmware overlay
# compiles (SEGACD_C_SOURCES minus its main) with the SAME device defines
# (SEGACD_C_DEFS verbatim), so what boots here is what the device runs.
#
# What it is NOT: a performance instrument. Host instructions are not device
# instructions; nothing here may be quoted as a cost.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROM="${1:?usage: run.sh <game.cue> [frames] [pad-pattern]}"
FRAMES="${2:-900}"
PAT="${3:-none}"
PD=external/picodrive
SPLIT="${SPLIT:-1}"
if [ "$SPLIT" = "1" ]; then
  OUT="${HOST_OUT:-tools/pico_host_segacd/build}"
  DEF="-DEMU_F68K -DFAMEC_CONST_JUMPTABLE -DFAMEC_NO_GOTOS -DGNW_CONST_TABLES -DGNW_MCD_SPLIT \
       -DGNW_MCD_BIOS_XIP -D_USE_CZ80 -DNDEBUG -DLSB_FIRST -DNO_32X -DNO_SMS \
       -ffunction-sections -fdata-sections"
else
  # Upstream (pre-split) memory model: inline bios/prg_ram in one mcd_state.
  # Isolates whether the fork's split-RAM path is what breaks retail boots.
  OUT="${HOST_OUT:-tools/pico_host_segacd/build_nosplit}"
  DEF="-DEMU_F68K -DFAMEC_CONST_JUMPTABLE -DFAMEC_NO_GOTOS -DGNW_CONST_TABLES \
       -D_USE_CZ80 -DNDEBUG -DLSB_FIRST -DNO_32X -DNO_SMS \
       -ffunction-sections -fdata-sections"
fi
mkdir -p "$OUT"
# --gc-sections is not cosmetic here. The device links with it (Makefile.common
# LDFLAGS), and that is exactly how pico/state.c's .gz path -- whose gzopen/
# gzread/... references nothing in this core defines -- disappears from the
# segacd overlay: segacd_save_state calls PicoStateFP directly, so open_save_file
# is an unreferenced section the linker discards. Without the same flags the
# host link would demand real zlib gz* symbols the device never ships.
INC="-I$PD -I$PD/pico -I$PD/cpu -I$PD/cpu/fame -I$PD/zlib"
SRCS="cpu/fame/famec.c cpu/cz80/cz80.c
      pico/pico.c pico/cart.c pico/memory.c pico/state.c pico/sek.c pico/z80if.c
      pico/videoport.c pico/draw.c pico/misc.c pico/eeprom.c pico/patch.c pico/media.c
      pico/pico/pico.c pico/pico/memory.c pico/pico/xpcm.c
      pico/carthw/carthw.c pico/carthw/eeprom_spi.c
      pico/cd/mcd.c pico/cd/memory.c pico/cd/sek.c pico/cd/cdc.c pico/cd/cdd.c
      pico/cd/cd_image.c pico/cd/cd_parse.c pico/cd/gfx.c pico/cd/gfx_dma.c
      pico/cd/misc.c pico/cd/pcm.c pico/cd/megasd.c
      pico/sound/sound.c pico/sound/mix.c pico/sound/sn76496.c
      pico/sound/ym2612.c pico/sound/resampler.c zlib/crc32.c"

objs=""
for s in $SRCS; do
  o="$OUT/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
  gcc -g -O1 -w -fcommon $DEF $INC -c "$PD/$s" -o "$o"
  objs="$objs $o"
done
for s in host_mcd host_mcd_stubs; do
  gcc -g -O1 -w -fcommon $DEF $INC -c "tools/pico_host_segacd/$s.c" -o "$OUT/$s.o"
  objs="$objs $OUT/$s.o"
done
gcc -o "$OUT/host_mcd" $objs -lm -Wl,--gc-sections
exec "$OUT/host_mcd" "$ROM" "$FRAMES" "$PAT"
