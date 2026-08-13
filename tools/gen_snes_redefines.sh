#!/bin/bash
# Regenerate snes_redefines: every global the generic-SNES overlay defines gets a
# gsnes__ prefix so it can NEVER alias the SM port's copies of the same sources
# (the g_snes saga: cross-overlay aliasing = silent corruption, 3 dead releases).
# Run after adding a source file to SNES_C_SOURCES or a global to main_snes.c;
# app_main_snes stays unrenamed (the launcher's entry point).
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
CC=arm-none-eabi-gcc
CF="-mcpu=cortex-m7 -mthumb -O1 -c -DTARGET_GNW -DGNW_SNES_CORE -DSNES_SPIN_SKIP -DSNES_SPIN_BAKE -Iexternal/sm/src -Iexternal/sm -ICore/Inc -w"
for f in apu cart cpu dma dsp input ppu snes snes_other spc spin_skip spin_bake; do
  $CC $CF external/sm/src/snes/$f.c -o "$T/$f.o"
done
$CC $CF external/sm/src/tracing.c -o "$T/tracing.o"
{
  arm-none-eabi-nm -g --defined-only "$T"/*.o | awk '{print $3}' | grep -v '^$'
  # main_snes.c's own globals (its firmware includes make it awkward to compile
  # here; the file is ours — keep this list in sync with its non-static symbols)
  echo CpuOpcodeHook; echo HookedFunctionRts; echo RtlApuWrite
  echo Die; echo Warning; echo g_fail; echo g_new_ppu
} | sort -u | awk '{print $1" gsnes__"$1}' > "$T/found"
# MERGE, never replace. This script compiles the core with its own small define
# set, which is NOT the build's: without -DSNES_THUMB2_CPU (and the rest) the
# objects have no cpu_runOpcode_c, no cpu_thumb2_fallback, no
# snes_cycles_per_opcode. A plain regeneration therefore DELETED 89 symbols
# from the alias guard -- and the next link failed on `multiple definition of
# snes_cycles_per_opcode` against the SM overlay, which is the exact collision
# this file exists to prevent. A guard that can silently shrink is not a guard.
cat snes_redefines "$T/found" 2>/dev/null | awk '!seen[$0]++' > "$T/merged"
mv "$T/merged" snes_redefines
rm -rf "$T"
echo "snes_redefines: $(wc -l < snes_redefines) symbols"
