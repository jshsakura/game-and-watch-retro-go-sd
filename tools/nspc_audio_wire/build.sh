#!/bin/bash
# Build the live-wired N-SPC HLE host harness. Same generated-copy discipline
# as tools/nspc_hle/build.sh (submodule untouched):
#   - spc_player.c -> parametrized dialect copy (nspc_hle pipeline) + export
#     the Spc_Loop tick (wire.c drives it at sample granularity)
#   - apu.c -> copy with apu_run renamed apu_run_lle (wire.c owns apu_run and
#     dispatches: LLE passthrough until the swap, native player after)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HERE=tools/nspc_audio_wire
HLE=tools/nspc_hle
O="${NSPC_WIRE_BUILD:-$ROOT/tools/nspc_audio_wire/build}"; mkdir -p "$O"; rm -f "$O"/*.o

# 1+2) generated sources (player copy + apu_wire) — shared with the firmware
bash "$HERE/gen_sources.sh" "$ROOT" "$O"

CORE="-O2 -g -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"
SMINC="-iquote external/sm/src"

for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  b="$(basename "$f")"
  [ "$b" = "apu.c" ] && continue                 # replaced by apu_wire.c
  gcc -c $CORE "$f" -o "$O/${b%.c}.o"
done
gcc -c $CORE -iquote external/sm/src/snes "$O/apu_wire.c" -o "$O/apu_wire.o"
gcc -c $CORE $SMINC -include tools/nspc_hle/nspc_config.h "$O/nspc_player_gen.c" -o "$O/nspc_player_gen.o"
gcc -c $CORE $SMINC -I"$HLE" "$HLE/nspc_variant.c" -o "$O/nspc_variant.o"
gcc -c $CORE $SMINC -I"$HLE" -I"$HERE" -Itools/snes_survey "$HERE/wire.c" -o "$O/wire.o"
gcc -c $CORE $SMINC -I"$HERE" "$HERE/host_main.c" -o "$O/host_main.o"
gcc -o "$O/wire_host" "$O"/*.o -lm
echo "BUILD OK -> $O/wire_host"
