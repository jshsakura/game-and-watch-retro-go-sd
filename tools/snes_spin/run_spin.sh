#!/bin/bash
# Generalized 65816 spin probe: WRAM/DP-flag spins, IO spins, WAI ticks.
# Build-time sed instruments copies of two submodule files (never edited on disk):
#   cpu.c  -> snes_spin_op(cpu) per opcode; snes_spin_read(cpu,adr) per bus read;
#             g_wai_ticks++ on the WAI waiting fast path
#   (cpu_write is in cpu.c too -> g_write_seq++)
# Usage: run_spin.sh <rom> [frames]
set -e
cd "$(dirname "$0")/../.."
O=/tmp/snes_spin; mkdir -p "$O"; rm -f "$O"/*.o
SM=external/sm/src

sed -e 's#return snes_cpuRead((Snes\*) cpu->mem, adr);#{ extern void snes_spin_read(Cpu*, uint32_t); snes_spin_read(cpu, adr); } return snes_cpuRead((Snes*) cpu->mem, adr);#' \
    -e 's#snes_cpuWrite((Snes\*) cpu->mem, adr, val);#{ extern unsigned long long g_write_seq; g_write_seq++; } snes_cpuWrite((Snes*) cpu->mem, adr, val);#' \
    -e 's#if(!(cpu->irqWanted || cpu->nmiWanted)) return 1;#if(!(cpu->irqWanted || cpu->nmiWanted)) { extern unsigned long long g_wai_ticks; g_wai_ticks++; return 1; }#' \
    -e 's#& 0xffffff\]++;#\& 0xffffff]++; { extern void snes_spin_op(Cpu*); snes_spin_op(cpu); }#' \
    "$SM/snes/cpu.c" > "$O/cpu.c"

# the seds must all have landed; a silent miss measures nothing
grep -q "snes_spin_read" "$O/cpu.c" || { echo "sed miss: spin_read"; exit 1; }
grep -q "g_write_seq"    "$O/cpu.c" || { echo "sed miss: write_seq"; exit 1; }
grep -q "g_wai_ticks"    "$O/cpu.c" || { echo "sed miss: wai_ticks"; exit 1; }
grep -q "snes_spin_op"   "$O/cpu.c" || { echo "sed miss: spin_op"; exit 1; }

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DSNES_PC_HISTOGRAM -w -Iexternal/sm -Iexternal/sm/src/snes -Itools/sm_harness/shim"
for f in $SM/snes/*.c; do
  b=$(basename "$f")
  [ "$b" = "cpu.c" ] && continue
  gcc -c $CF "$f" -o "$O/${b%.c}.o"
done
gcc -c $CF "$O/cpu.c" -o "$O/cpu.o"
gcc -c $CF "$SM/tracing.c" -o "$O/tracing.o"
gcc -c $CF tools/snes_spin/spin_probe.c -o "$O/spin_probe.o"
gcc -o "$O/spin" "$O"/*.o -lm
"$O/spin" "$1" "${2:-2000}"
