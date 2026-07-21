#!/bin/bash
# 65816 static-recompiler feasibility probe.
#   bash tools/sfc_recomp_probe/run_probe.sh <rom.smc> [frames=2000]
#
# Builds the host SNES harness with an instrumented COPY of cpu.c (the submodule
# is never edited): probe_op/probe_post hooks around cpu_doOpcode record the
# executed-code map, flag polymorphism, ROM/WRAM split, block leaders and
# indirect-jump target sets. snes_main.c is copied and given the Start-tap so
# the ROM walks from title into gameplay.
set -euo pipefail
cd "$(dirname "$0")/../.."

ROM="${1:?usage: run_probe.sh <rom.smc> [frames]}"
FRAMES="${2:-2000}"

O=/tmp/sfc_recomp_probe_build
mkdir -p "$O"; rm -f "$O"/*.o

# 1. instrumented cpu.c: hook the single cpu_doOpcode call site in cpu_runOpcode
sed 's/  cpu_doOpcode(cpu, opcode);/  { extern void probe_op(Cpu*,uint8_t); probe_op(cpu, opcode); }\n  cpu_doOpcode(cpu, opcode);\n  { extern void probe_post(Cpu*); probe_post(cpu); }/' \
    external/sm/src/snes/cpu.c > "$O/cpu_probe.c"
grep -q probe_op "$O/cpu_probe.c" || { echo "sed hook failed"; exit 1; }

# 2. harness copy: Start-tap input so the game reaches gameplay; report at exit
sed -e 's/    snes->input1->currentState = 0;/    snes->input1->currentState = (i >= 40 \&\& (i % 24) < 6) ? 0x0008 : 0;/' \
    -e 's/^  return 0;$/  { extern void probe_report(void); probe_report(); }\n  return 0;/' \
    tools/snes_harness/snes_main.c > "$O/probe_main.c"
grep -q probe_report "$O/probe_main.c" || { echo "main sed failed"; exit 1; }
grep -q '0x0008' "$O/probe_main.c" || { echo "input sed failed"; exit 1; }

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"

for f in external/sm/src/snes/*.c; do
  base="$(basename "$f")"
  [ "$base" = "cpu.c" ] && continue          # replaced by the instrumented copy
  gcc -c $CF "$f" -o "$O/${base%.c}.o"
done
gcc -c $CF external/sm/src/tracing.c -o "$O/tracing.o"
gcc -c $CF -Iexternal/sm/src/snes "$O/cpu_probe.c"  -o "$O/cpu_probe.o"   # copy lives in /tmp; resolve its sibling includes
gcc -c $CF tools/sfc_recomp_probe/probe.c -o "$O/probe.o"
gcc -c $CF "$O/probe_main.c" -o "$O/probe_main.o"
gcc -o "$O/probe" "$O"/*.o -lm

"$O/probe" "$ROM" "$FRAMES"
