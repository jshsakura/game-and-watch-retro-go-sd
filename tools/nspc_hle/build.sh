#!/bin/bash
# Build the N-SPC HLE proof-of-concept. Generalizes SM's spc_player.c WITHOUT
# editing the submodule: a build-time sed rewrites its 4 hardcoded ARAM addresses
# into runtime-config macros (nspc_config.h), emitting a generated copy we compile.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
HERE=tools/nspc_hle
O=/tmp/nspc_hle_build; mkdir -p "$O"; rm -f "$O"/*.o

# 1) parametrize spc_player.c (submodule stays untouched)
sed -e 's/instrument \* 6 + 0x6c00/instrument * 6 + NSPC_INSTR/' \
    -e 's/p->ram\[0x5820 + (a - 1) \* 2\]/p->ram[NSPC_SONGLIST + (a - 1) * 2]/' \
    -e 's/p->ram\[0x581e\]/p->ram[NSPC_SONGCUR]/' \
    -e 's/Dsp_Write(p, DIR, 0x6d)/Dsp_Write(p, DIR, NSPC_DIRPAGE)/' \
    external/sm/src/spc_player.c > "$O/nspc_player_gen.c"
for tok in NSPC_INSTR NSPC_SONGLIST NSPC_SONGCUR NSPC_DIRPAGE; do
  grep -q "$tok" "$O/nspc_player_gen.c" || { echo "SED MISS: $tok"; exit 1; }
done

# 2) dialect + KonamiBase + loop-guard rewrites (exact anchors, abort on miss)
python3 "$HERE/gen_variant.py" "$O/nspc_player_gen.c"

# core needs -Iexternal/sm only (its files include "src/snes/..."); adding
# external/sm/src to the core compile shadows headers and breaks it.
CORE="-O2 -DNDEBUG -DTARGET_GNW -DGNW_SNES_CORE -DHEADLESS -w -Iexternal/sm -Itools/sm_harness/shim"
# the SM player/harness include "spc_player.h","snes/spc.h" -> need external/sm/src
SMINC="-iquote external/sm/src"

for f in external/sm/src/snes/*.c external/sm/src/tracing.c; do
  gcc -c $CORE "$f" -o "$O/$(basename ${f%.c}).o"
done
gcc -c $CORE $SMINC -include "$HERE/nspc_config.h" "$O/nspc_player_gen.c" -o "$O/nspc_player_gen.o"
gcc -c $CORE $SMINC -I"$HERE" "$HERE/nspc_variant.c" -o "$O/nspc_variant.o"
gcc -c $CORE $SMINC -I"$HERE" -Itools/snes_survey "$HERE/nspc_poc.c" -o "$O/poc.o"
gcc -o "$O/nspc_poc" "$O"/*.o -lm
echo "BUILD OK -> $O/nspc_poc"
