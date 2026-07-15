#!/bin/bash
# Measure the 65816 APU-wait spin fraction (idle-skip fps-ceiling probe).
# Build-time sed instruments two submodule files (never edited on disk):
#   snes.c  -> flag g_apu_read when the 65816 reads an APU out-port $2140-$2143
#   cpu.c   -> call snes_idle_op() once per opcode (inside the histogram hook)
# Usage: run_idle.sh <rom> [frames]
set -e
cd "$(dirname "$0")/../.."
O=/tmp/snes_idle; mkdir -p "$O"; rm -f "$O"/*.o
SM=external/sm/src

# instrumented copies
sed 's#return snes->apu->outPorts\[adr & 0x3\];#{ extern int g_apu_read; if (adr <= 0x43) g_apu_read = 1; } return snes->apu->outPorts[adr \& 0x3];#' \
    "$SM/snes/snes.c" > "$O/snes.c"
sed 's#& 0xffffff\]++;#\& 0xffffff]++; { extern void snes_idle_op(uint32_t); snes_idle_op((((uint32_t)cpu->k << 16) | (uint16_t)(cpu->pc - 1)) \& 0xffffff); }#' \
    "$SM/snes/cpu.c" > "$O/cpu.c"

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DSNES_PC_HISTOGRAM -w -Iexternal/sm -Iexternal/sm/src/snes -Itools/sm_harness/shim"
# core, but swap snes.c and cpu.c for the instrumented copies
for f in $SM/snes/*.c; do
  b=$(basename "$f")
  [ "$b" = "snes.c" ] && continue
  [ "$b" = "cpu.c" ]  && continue
  gcc -c $CF "$f" -o "$O/${b%.c}.o"
done
gcc -c $CF "$O/snes.c" -o "$O/snes.o"
gcc -c $CF "$O/cpu.c"  -o "$O/cpu.o"
gcc -c $CF "$SM/tracing.c" -o "$O/tracing.o"
gcc -c $CF tools/snes_idle/idle_probe.c -o "$O/idle_probe.o"
gcc -o "$O/idle" "$O"/*.o -lm
"$O/idle" "$1" "${2:-1200}"
