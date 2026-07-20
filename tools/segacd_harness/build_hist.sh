#!/bin/bash
# build_hist.sh — build the SegaCD host boot harness WITH the 32X-style
# per-opcode/per-PC histogram (HOOK_CPU path in gwenesis Musashi).
#
# Output: /tmp/boot_hist  (run: /tmp/boot_hist <bios> <cue> [frames])
#
# Diff vs the canonical build line in
# Core/Src/porting/segacd/CLAUDE.md / session-handoff-0716-segacd.md:
#   + -DHOOK_CPU                              (enable the dispatch-loop hook)
#   + -Itools/segacd_harness                  (finds cpuhook.h included by m68k.h)
#   + tools/segacd_harness/scd_hist.c         (defines cpu_hook + histograms)
#
# The hook stays zero-cost on the device: HOOK_CPU is harness-only and the
# firmware build does not define it.
#
# Run from the gnw-segacd worktree root:
#   bash tools/segacd_harness/build_hist.sh && /tmp/boot_hist /tmp/scd/bios_CD_U.bin "$(ls /tmp/scd/*.cue|head -1)" 250
set -euo pipefail
GW=external/gwenesis/src
SC=Core/Src/porting/segacd
HIST=tools/segacd_harness

gcc -O2 -DHOOK_CPU -DSEGACD_GA_TRACE -DLINUX_EMU -DTARGET_GNW -DIS_LITTLE_ENDIAN \
  -DSD_CARD=0 -DCHEAT_CODES=0 -DLSB_FIRST -DTABLES_FULL \
  -Itools/m7_qemu_rig/md_shim -I$GW/cpus/M68K -I$GW/cpus/Z80 -I$GW/sound \
  -I$GW/bus -I$GW/vdp -I$GW/io -I$GW/savestate -ICore/Inc -ICore/Inc/retro-go \
  -ICore/Src/porting/lib -Iretro-go-stm32/components/odroid -I$SC \
  -I$HIST \
  $HIST/boot_test.c $HIST/scd_hist.c \
  $SC/segacd_engine.c $SC/segacd_bus.c $SC/segacd_cd.c $SC/segacd_audio.c \
  $SC/segacd_gfx.c \
  $GW/cpus/M68K/m68kcpu.c $GW/cpus/Z80/Z80.c $GW/sound/z80inst.c $GW/sound/ym2612.c \
  $GW/sound/gwenesis_sn76489.c $GW/bus/gwenesis_bus.c $GW/bus/gwenesis_sram.c \
  $GW/bus/gwenesis_eeprom.c $GW/io/gwenesis_io.c $GW/vdp/gwenesis_vdp_mem.c \
  $GW/vdp/gwenesis_vdp_gfx.c $GW/savestate/gwenesis_savestate.c \
  -lm -o /tmp/boot_hist 2>/tmp/build_hist.err
status=$?
if [ $status -ne 0 ]; then
  echo "BUILD FAILED (rc=$status):"
  cat /tmp/build_hist.err
  exit $status
fi
# surface any warnings too
if [ -s /tmp/build_hist.err ]; then
  echo "--- build warnings ---"
  cat /tmp/build_hist.err
fi
echo "BUILD_OK -> /tmp/boot_hist"
