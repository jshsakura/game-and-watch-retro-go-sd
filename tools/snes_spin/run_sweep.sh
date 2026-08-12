#!/bin/bash
# Spin-loop sweep across a ROM directory: is the "81% skippable spin" finding
# (Zelda) a library-wide property or an RPG one?
#
# Builds the spin probe ONCE (same instrumented-copy build as run_spin.sh),
# then runs every .smc/.sfc in parallel with a per-ROM timeout, one TSV row
# per ROM:
#   rom \t status \t lit \t pure% \t io% \t wai_per_frame \t top_site_pc \t top_polled_addr
# status: OK | UNRENDERED (never lit a pixel; numbers are boot-phase) |
#         LOAD_FAIL | BOOT_CRASH | TIMEOUT
#
# Usage: run_sweep.sh <rom-dir> [frames=1200] [out.tsv=/tmp/snes_spin_sweep.tsv]
set -e
cd "$(dirname "$0")/../.."
ROMDIR="${1:?usage: run_sweep.sh <rom-dir> [frames] [out.tsv]}"
FRAMES="${2:-1200}"
OUT="${3:-/tmp/snes_spin_sweep.tsv}"

O=/tmp/snes_spin_sweep_build; mkdir -p "$O"; rm -f "$O"/*.o
SM=external/sm/src

# --- build (identical instrumentation to run_spin.sh; sed-copies, submodule untouched)
sed -e 's#return snes_cpuRead((Snes\*) cpu->mem, adr);#{ extern void snes_spin_read(Cpu*, uint32_t); snes_spin_read(cpu, adr); } return snes_cpuRead((Snes*) cpu->mem, adr);#' \
    -e 's#snes_cpuWrite((Snes\*) cpu->mem, adr, val);#{ extern unsigned long long g_write_seq; g_write_seq++; } snes_cpuWrite((Snes*) cpu->mem, adr, val);#' \
    -e 's#if(!(cpu->irqWanted || cpu->nmiWanted)) return 1;#if(!(cpu->irqWanted || cpu->nmiWanted)) { extern unsigned long long g_wai_ticks; g_wai_ticks++; return 1; }#' \
    -e 's#& 0xffffff\]++;#\& 0xffffff]++; { extern void snes_spin_op(Cpu*); snes_spin_op(cpu); }#' \
    "$SM/snes/cpu.c" > "$O/cpu.c"
for tok in snes_spin_read g_write_seq g_wai_ticks snes_spin_op; do
  grep -q "$tok" "$O/cpu.c" || { echo "sed miss: $tok"; exit 1; }
done

CF="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -DSNES_PC_HISTOGRAM -w -Iexternal/sm -Iexternal/sm/src/snes -Itools/sm_harness/shim"
for f in $SM/snes/*.c; do
  b=$(basename "$f"); [ "$b" = "cpu.c" ] && continue
  gcc -c $CF "$f" -o "$O/${b%.c}.o"
done
gcc -c $CF "$O/cpu.c" -o "$O/cpu.o"
gcc -c $CF "$SM/tracing.c" -o "$O/tracing.o"
gcc -c $CF tools/snes_spin/spin_probe.c -o "$O/spin_probe.o"
gcc -o "$O/spin" "$O"/*.o -lm
echo "built $O/spin" >&2

# --- one ROM -> one TSV row (runs under xargs -P; writes to its own file)
run_one() {
  local rom="$1" frames="$2" outdir="$3"
  local base; base="$(basename "$rom")"
  local raw="$outdir/$(echo "$base" | md5sum | cut -c1-12).raw"
  local rc=0
  timeout 60 "$O/spin" "$rom" "$frames" > "$raw" 2>/dev/null || rc=$?

  local status="OK" lit="-" pure="-" io="-" wai="-" site="-" polls="-"
  if [ "$rc" = "124" ]; then
    status="TIMEOUT"
  elif grep -q "unsupported ROM" "$raw"; then
    status="LOAD_FAIL"
  elif ! grep -q "^\[spin\]" "$raw"; then
    status="BOOT_CRASH"
  else
    lit=$(grep -o "lit=[0-9]*" "$raw" | head -1 | cut -d= -f2)
    pure=$(grep "^\[spin\]" "$raw" | grep -o "PURE-spin=[0-9.]*" | cut -d= -f2)
    # ops/frame: the probe already computes it and only the TSV was missing it.
    # pure% alone cannot rank candidates -- it is the share of opcodes that are
    # replayable, and the prize is that share of the interpreter's total cost
    # (+2.5 fps by ablation). Whether the prize is REALISABLE needs to know if
    # the ROM is below the audio cap at all, and ops/frame is the proxy for it.
    ops=$(grep "^\[spin\]" "$raw" | grep -o "ops/frame=[0-9.]*" | cut -d= -f2)
    io=$(grep "^\[spin\]" "$raw" | grep -o "IO-spin=[0-9.]*" | cut -d= -f2)
    wai=$(grep "^\[spin\]" "$raw" | grep -o "WAIticks/frame=[0-9.]*" | cut -d= -f2)
    local sl; sl=$(grep "^\[site\]" "$raw" | head -1)
    if [ -n "$sl" ]; then
      site=$(echo "$sl" | awk '{print $2}')
      polls=$(echo "$sl" | grep -o "polls=\$[0-9a-f]*" | cut -d\$ -f2)
    fi
    [ "${lit:-0}" = "0" ] && status="UNRENDERED"
  fi
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$base" "$status" "$lit" "$pure" "$io" "$wai" "$site" "$polls" "$ops" > "$raw.tsv"
}
export -f run_one; export O

TMP=$(mktemp -d /tmp/spin_sweep.XXXXXX)
find "$ROMDIR" -maxdepth 2 -type f \( -iname '*.smc' -o -iname '*.sfc' \) -print0 \
  | xargs -0 -P "$(nproc)" -I{} bash -c 'run_one "$1" "$2" "$3"' _ {} "$FRAMES" "$TMP"

cat "$TMP"/*.tsv | sort > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") ROMs)" >&2
