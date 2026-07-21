#!/usr/bin/env bash
# Exact host correctness gate for the committed production 270-site SMW RC.
# Builds only temporary host executables; never invokes make/release/Docker.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"

frames=${1:-1500}
smw=${SMW_ROM:-external/smw/smw.sfc}
zelda=${ZELDA_ROM:-roms/zelda_alttp.smc}
out=$(mktemp -d /tmp/snes-rc-hot-correctness.XXXXXX)
keep=${KEEP_RC_HOT_LOGS:-}
trap 'if [[ -z ${keep:-} ]]; then rm -rf "$out"; else echo "logs kept: $out" >&2; fi' EXIT

for rom in "$smw" "$zelda"; do
  [[ -f "$rom" ]] || { echo "FAIL missing ROM: $rom" >&2; exit 2; }
done

cf=(-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w
    -ffp-contract=off -Iexternal/sm -Itools/sm_harness/shim)
core_sources=(
  external/sm/src/snes/apu.c external/sm/src/snes/cart.c
  external/sm/src/snes/dma.c external/sm/src/snes/dsp.c
  external/sm/src/snes/input.c external/sm/src/snes/ppu.c
  external/sm/src/snes/snes.c external/sm/src/snes/snes_other.c
  external/sm/src/snes/spc.c external/sm/src/tracing.c
  external/sm/src/snes/rc_dispatch.c
)
core_objects=()

echo "== compile common production core" >&2
for source in "${core_sources[@]}"; do
  object="$out/$(basename "${source%.c}").o"
  gcc -c "${cf[@]}" "$source" -o "$object"
  core_objects+=("$object")
done

gcc -c "${cf[@]}" external/sm/src/snes/cpu.c -o "$out/cpu.o"
gcc -c "${cf[@]}" tools/sfc_recomp/harness_main.c -o "$out/harness.o"
gcc -o "$out/baseline" "${core_objects[@]}" "$out/cpu.o" "$out/harness.o" -lm

# Production rc_smw_sites.c includes cpu_copy.c and the committed
# generated/rc_smw/rc_sites.inc. cpu_copy is a generated build artifact, not a
# source-tree edit.
sed 's/^int cpu_runOpcode(Cpu\* cpu) {/static int rc_orig_runOpcode(Cpu* cpu) {/' \
  external/sm/src/snes/cpu.c > "$out/cpu_copy.c"
grep -q 'rc_orig_runOpcode' "$out/cpu_copy.c" || {
  echo "FAIL cpu_copy cpu_runOpcode rename" >&2; exit 1;
}
gcc -c "${cf[@]}" -I"$out" -Igenerated/rc_smw -Iexternal/sm/src/snes \
  Core/Src/porting/snes/rc_smw_sites.c -o "$out/rc_smw_sites.o"
gcc -c "${cf[@]}" tools/gnw_hw_harness/rc_hot_host_activate.c \
  -o "$out/rc_hot_host_activate.o"
gcc -o "$out/candidate" "${core_objects[@]}" "$out/cpu.o" "$out/harness.o" \
  "$out/rc_smw_sites.o" "$out/rc_hot_host_activate.o" -lm \
  -Wl,--wrap=snes_loadRom -Wl,--wrap=rc_dispatch_lookup

extract_hashes() {
  sed -n 's/.*state=\([0-9a-fA-F]*\)  audio=\([0-9a-fA-F]*\).*/\1 \2/p' "$1" | tail -1
}

run_case() {
  local name=$1 rom=$2
  echo "== $name baseline ($frames frames)" >&2
  "$out/baseline" "$rom" "$frames" >"$out/$name.base.log" 2>&1
  echo "== $name 270-site candidate ($frames frames)" >&2
  "$out/candidate" "$rom" "$frames" >"$out/$name.hot.log" 2>&1

  local base hot
  base=$(extract_hashes "$out/$name.base.log")
  hot=$(extract_hashes "$out/$name.hot.log")
  [[ -n "$base" && -n "$hot" ]] || {
    echo "FAIL $name: missing state/audio hash" >&2
    tail -20 "$out/$name.base.log" >&2
    tail -20 "$out/$name.hot.log" >&2
    exit 1
  }

  if [[ "$base" != "$hot" ]]; then
    echo "FAIL $name: baseline state/audio=[$base], hot=[$hot]" >&2
    exit 1
  fi
  echo "PASS $name: state/audio=$hot bit-identical"
}

run_case SMW "$smw"
grep -Eq '\[rc-validate\] active nsites=270 codehash=[0-9a-fA-F]{8}' "$out/SMW.hot.log" || {
  echo "FAIL SMW: candidate did not activate exactly 270 sites" >&2; exit 1;
}
smw_hits=$(sed -n 's/.*active=1 native_hits=\([0-9]*\).*/\1/p' "$out/SMW.hot.log" | tail -1)
[[ ${smw_hits:-0} -gt 0 ]] || {
  echo "FAIL SMW: candidate activated but recorded no native hits" >&2; exit 1;
}
echo "PASS SMW activation: 270 sites, native_hits=$smw_hits"

run_case Zelda "$zelda"
grep -q '\[rc-validate\] inactive title=' "$out/Zelda.hot.log" || {
  echo "FAIL Zelda: SMW identity gate did not reject the ROM" >&2; exit 1;
}
grep -q '\[rc-validate\] active=0 native_hits=0 cold_fallbacks=0' "$out/Zelda.hot.log" || {
  echo "FAIL Zelda: RC lookup ran despite inactive SMW identity gate" >&2; exit 1;
}
echo "PASS Zelda activation: SMW candidate inactive, interpreter-only"

echo "PASS snes-rc-hot correctness: SMW+Zelda state/audio bit-identical"
