#!/bin/bash
# build_cache.sh — build the SegaCD host boot harness with $7c80 rotation cache.
# Uses HOOK_CPU + SCD_CACHE + SCD_CACHE_ONLY: the per-instruction hook intercepts
# $7c80 entries, compares 64-byte input, skips on cache HIT (restore intermediates,
# pop return address, consume ~500 cycles). No histogram arrays (8MB each) compiled.
#
# Output: /tmp/boot_cache  (run via bench.sh with SCD_* env toggles)
set -uo pipefail
GW=external/gwenesis/src
SC=Core/Src/porting/segacd
HIST=tools/segacd_harness

gcc -O2 -DLINUX_EMU -DTARGET_GNW -DIS_LITTLE_ENDIAN \
  -DSD_CARD=0 -DCHEAT_CODES=0 -DLSB_FIRST -DTABLES_FULL \
  -DSCD_BENCH_VDP \
  -DHOOK_CPU -DSCD_CACHE -DSCD_CACHE_ONLY \
  -Itools/m7_qemu_rig/md_shim -I$GW/cpus/M68K -I$GW/cpus/Z80 -I$GW/sound \
  -I$GW/bus -I$GW/vdp -I$GW/io -I$GW/savestate -ICore/Inc -ICore/Inc/retro-go \
  -ICore/Src/porting/lib -Iretro-go-stm32/components/odroid -I$SC -I$HIST \
  $HIST/boot_test.c \
  $HIST/scd_hist.c \
  $SC/segacd_engine.c $SC/segacd_bus.c $SC/segacd_cd.c $SC/segacd_audio.c \
  $SC/segacd_gfx.c \
  $GW/cpus/M68K/m68kcpu.c $GW/cpus/Z80/Z80.c $GW/sound/z80inst.c $GW/sound/ym2612.c \
  $GW/sound/gwenesis_sn76489.c $GW/bus/gwenesis_bus.c $GW/bus/gwenesis_sram.c \
  $GW/bus/gwenesis_eeprom.c $GW/io/gwenesis_io.c $GW/vdp/gwenesis_vdp_mem.c \
  $GW/vdp/gwenesis_vdp_gfx.c $GW/savestate/gwenesis_savestate.c \
  -lm -o /tmp/boot_cache 2>/tmp/build_cache.err
status=$?
if [ $status -ne 0 ]; then
  echo "BUILD FAILED (rc=$status):"
  cat /tmp/build_cache.err
  exit $status
fi
if [ -s /tmp/build_cache.err ]; then
  echo "--- build warnings ---"
  cat /tmp/build_cache.err | grep -v 'warn_unused_result\|fread' | head -10
fi
echo "BUILD_OK -> /tmp/boot_cache"
