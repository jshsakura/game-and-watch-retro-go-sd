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
O=/tmp/nspc_wire_build; mkdir -p "$O"; rm -f "$O"/*.o

# 1) parametrized player copy (address sed + dialect rewrites, from nspc_hle)
sed -e 's/instrument \* 6 + 0x6c00/instrument * 6 + NSPC_INSTR/' \
    -e 's/p->ram\[0x5820 + (a - 1) \* 2\]/p->ram[NSPC_SONGLIST + (a - 1) * 2]/' \
    -e 's/p->ram\[0x581e\]/p->ram[NSPC_SONGCUR]/' \
    -e 's/Dsp_Write(p, DIR, 0x6d)/Dsp_Write(p, DIR, NSPC_DIRPAGE)/' \
    external/sm/src/spc_player.c > "$O/nspc_player_gen.c"
for tok in NSPC_INSTR NSPC_SONGLIST NSPC_SONGCUR NSPC_DIRPAGE; do
  grep -q "$tok" "$O/nspc_player_gen.c" || { echo "SED MISS: $tok"; exit 1; }
done
python3 "$HLE/gen_variant.py" "$O/nspc_player_gen.c"

# 1a2 REMOVED (was: rewrite $00nn nn>=0x80 top-level phrase words into an
# unconditional jump, on a theory that std N-SPC treats them that way and SM
# "repurposed" 0x80/0x81 as fast-forward toggles). Checked against BOTH
# independent std-family decompilations that exist in this tree --
# external/sm/src/spc_player.c (Super Metroid) and external/zelda3/spc_player.c
# (ALttP's own decompile, the very game this patch was written to fix) -- and
# they agree with each other byte-for-byte on this branch: $0080 sets
# fast_forward=0x80, $0081 clears it, nothing here is a jump. The jump
# mechanics live in the *next* branch down (the block_count/loop-address
# case) for every other nonzero low byte. So the patch encoded a semantics no
# known std engine implements, and this host generator was the only one of
# the two N-SPC pipelines applying it -- gen_nspc_wire.py (the device/firmware
# generator, Makefile.common's SNES_NSPC_HLE=1 path) never had it. Verified
# with tools/nspc_audio_wire's host + device harnesses: removing this step
# changes nothing for Zelda/SM/MegaManX/EarthBound (identical fb hash + audio
# rms in every window) -- the divergence was real but never observed by any
# of these four ROMs, i.e. cosmetic for them. See
# fix/nspc-generator-divergence for the full writeup.

# 1b) export the tick so wire.c can step samples (SpcPlayer_GenerateSamples
#     semantics at apu_run granularity)
sed -i -e 's/^static void Spc_Loop_Part1(SpcPlayer \*p) {/void Spc_Loop_Part1(SpcPlayer *p) {/' \
       -e 's/^static void Spc_Loop_Part2(SpcPlayer \*p, uint8 ticks) {/void Spc_Loop_Part2(SpcPlayer *p, uint8 ticks) {/' \
       "$O/nspc_player_gen.c"
grep -q '^void Spc_Loop_Part1' "$O/nspc_player_gen.c" || { echo "SED MISS: Spc_Loop_Part1"; exit 1; }
grep -q '^void Spc_Loop_Part2' "$O/nspc_player_gen.c" || { echo "SED MISS: Spc_Loop_Part2"; exit 1; }

# 2) apu.c copy: original apu_run steps aside for wire.c's dispatcher
sed 's/^void apu_run(Apu\* apu, int cyclesToRun) {/void apu_run_lle(Apu* apu, int cyclesToRun) {/' \
    external/sm/src/snes/apu.c > "$O/apu_wire.c"
grep -q '^void apu_run_lle' "$O/apu_wire.c" || { echo "SED MISS: apu_run_lle"; exit 1; }

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
