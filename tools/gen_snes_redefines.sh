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
CF="-mcpu=cortex-m7 -mthumb -O1 -c -DTARGET_GNW -DGNW_SNES_CORE -DSNES_SPIN_SKIP -Iexternal/sm/src -Iexternal/sm -ICore/Inc -w"
for f in apu cart cpu dma dsp input ppu snes snes_other spc spin_skip; do
  $CC $CF external/sm/src/snes/$f.c -o "$T/$f.o"
done
$CC $CF external/sm/src/tracing.c -o "$T/tracing.o"
{
  arm-none-eabi-nm -g --defined-only "$T"/*.o | awk '{print $3}' | grep -v '^$'
  # main_snes.c's own globals (its firmware includes make it awkward to compile
  # here; the file is ours — keep this list in sync with its non-static symbols)
  echo CpuOpcodeHook; echo HookedFunctionRts; echo RtlApuWrite
  echo Die; echo Warning; echo g_fail; echo g_new_ppu
} | sort -u | awk '{print $1" gsnes__"$1}' > snes_redefines
rm -rf "$T"
echo "snes_redefines: $(wc -l < snes_redefines) symbols"
