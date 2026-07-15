#!/bin/bash
# Build the exact-replay spin-skip harness and gate it: SNES_SKIP=0 vs 1 must
# print identical state/audio hashes. Usage: run_skip.sh <rom> [frames]
set -e
cd "$(dirname "$0")/../.."
O=/tmp/snes_skip; mkdir -p "$O"; rm -f "$O"/*.o
SM=external/sm/src

# purity counters only (no per-opcode hook — keeps the timing measurement clean)
sed -e 's#return snes_cpuRead((Snes\*) cpu->mem, adr);#{ extern void snes_spin_read(Cpu*, uint32_t); snes_spin_read(cpu, adr); } return snes_cpuRead((Snes*) cpu->mem, adr);#' \
    -e 's#snes_cpuWrite((Snes\*) cpu->mem, adr, val);#{ extern unsigned long long g_write_seq; g_write_seq++; } snes_cpuWrite((Snes*) cpu->mem, adr, val);#' \
    "$SM/snes/cpu.c" > "$O/cpu.c"
grep -q "snes_spin_read" "$O/cpu.c" || { echo "sed miss: spin_read"; exit 1; }
grep -q "g_write_seq"    "$O/cpu.c" || { echo "sed miss: write_seq"; exit 1; }

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Iexternal/sm/src/snes -Itools/sm_harness/shim"
for f in $SM/snes/*.c; do
  b=$(basename "$f")
  [ "$b" = "cpu.c" ] && continue
  gcc -c $CF "$f" -o "$O/${b%.c}.o"
done
gcc -c $CF "$O/cpu.c" -o "$O/cpu.o"
gcc -c $CF "$SM/tracing.c" -o "$O/tracing.o"
gcc -c $CF tools/snes_spin/skip_harness.c -o "$O/skip.o"
gcc -o "$O/skip" "$O"/*.o -lm

echo "--- baseline (skip off) ---"
SNES_SKIP=0 "$O/skip" "$1" "${2:-1500}"
echo "--- skip on ---"
SNES_SKIP=1 "$O/skip" "$1" "${2:-1500}"
