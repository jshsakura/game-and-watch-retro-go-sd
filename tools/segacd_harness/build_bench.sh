#!/bin/bash
# build_bench.sh — build the SegaCD host boot harness for BUDGET timing only.
# Variant of build_hist.sh with NO instrumentation flags so wall-clock reflects
# the unmodified boot path (no per-instruction histogram, no GA counters).
#
# Output: /tmp/boot_bench  (run via bench.sh, which sets SCD_* env toggles)
#
# Diff vs build_hist.sh:
#   - no -DHOOK_CPU           (no per-instruction cpu_hook, no scd_hist.c)
#   - no -DSEGACD_GA_TRACE    (no GA counter increments in segacd_*.c)
set -uo pipefail
GW=external/gwenesis/src
SC=Core/Src/porting/segacd
HIST=tools/segacd_harness

gcc -O2 -DLINUX_EMU -DTARGET_GNW -DIS_LITTLE_ENDIAN \
  -DSD_CARD=0 -DCHEAT_CODES=0 -DLSB_FIRST -DTABLES_FULL \
  -DSCD_BENCH_VDP -DSCD_Z80_IDLE_SKIP \
  -Itools/m7_qemu_rig/md_shim -I$GW/cpus/M68K -I$GW/cpus/Z80 -I$GW/sound \
  -I$GW/bus -I$GW/vdp -I$GW/io -I$GW/savestate -ICore/Inc -ICore/Inc/retro-go \
  -ICore/Src/porting/lib -Iretro-go-stm32/components/odroid -I$SC -I$HIST \
  $HIST/boot_test.c \
  $SC/segacd_engine.c $SC/segacd_bus.c $SC/segacd_cd.c $SC/segacd_audio.c \
  $SC/segacd_gfx.c \
  $GW/cpus/M68K/m68kcpu.c $GW/cpus/Z80/Z80.c $GW/sound/z80inst.c $GW/sound/ym2612.c \
  $GW/sound/gwenesis_sn76489.c $GW/bus/gwenesis_bus.c $GW/bus/gwenesis_sram.c \
  $GW/bus/gwenesis_eeprom.c $GW/io/gwenesis_io.c $GW/vdp/gwenesis_vdp_mem.c \
  $GW/vdp/gwenesis_vdp_gfx.c $GW/savestate/gwenesis_savestate.c \
  -lm -o /tmp/boot_bench 2>/tmp/build_bench.err
status=$?
if [ $status -ne 0 ]; then
  echo "BUILD FAILED (rc=$status):"
  cat /tmp/build_bench.err
  exit $status
fi
if [ -s /tmp/build_bench.err ]; then
  echo "--- build warnings ---"
  cat /tmp/build_bench.err | grep -v 'warn_unused_result\|fread' | head -10
fi
echo "BUILD_OK -> /tmp/boot_bench"
