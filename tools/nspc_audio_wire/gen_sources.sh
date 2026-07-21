#!/bin/bash
# Generate the wire's two derived sources into <outdir> (submodule untouched):
#   nspc_player_gen.c — external/sm/src/spc_player.c parametrized for foreign
#                       N-SPC dialects (addresses -> nspc_config macros, dialect
#                       rewrites, std phrase-jump semantics, exported tick,
#                       allocation behind nspc_player_storage()).
#   apu_wire.c        — apu.c with apu_run renamed apu_run_lle (wire.c owns
#                       apu_run and dispatches LLE/native).
# Shared by the host harness build (build.sh) and the firmware Makefile.
#   gen_sources.sh <repo_root> <outdir>
set -euo pipefail
ROOT="${1:?usage: gen_sources.sh <repo_root> <outdir>}"
O="${2:?usage: gen_sources.sh <repo_root> <outdir>}"
cd "$ROOT"
HLE=tools/nspc_hle
mkdir -p "$O"

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

# 1a) standard N-SPC phrase semantics: $00nn with nn>=0x80 is a JUMP (loop
#     forever to the address that follows). SM's engine repurposed 0x80/0x81
#     as fast-forward toggles — under that reading ALttP's title-loop gets
#     fast-forwarded into the stop path (song plays once, then silence).
python3 - "$O/nspc_player_gen.c" <<'PYEOF'
import sys
p = sys.argv[1]
src = open(p).read()
old = """      if (t == 0x80) {
        p->fast_forward = 0x80;
      } else if (t == 0x81) {
        p->fast_forward = 0;
      } else {"""
new = """      if (t >= 0x80) {   /* std N-SPC: $00nn nn>=0x80 = jump (loop) */
        t = WORD(p->ram[p->music_ptr_toplevel]);
        p->music_ptr_toplevel = t;
      } else {"""
assert src.count(old) == 1, "phrase-branch anchor miss"
open(p, "w").write(src.replace(old, new))
print("phrase-jump semantics applied")
PYEOF

# 1b) export the tick so wire.c can step samples (SpcPlayer_GenerateSamples
#     semantics at apu_run granularity)
sed -i -e 's/^static void Spc_Loop_Part1(SpcPlayer \*p) {/void Spc_Loop_Part1(SpcPlayer *p) {/' \
       -e 's/^static void Spc_Loop_Part2(SpcPlayer \*p, uint8 ticks) {/void Spc_Loop_Part2(SpcPlayer *p, uint8 ticks) {/' \
       "$O/nspc_player_gen.c"
grep -q '^void Spc_Loop_Part1' "$O/nspc_player_gen.c" || { echo "SED MISS: Spc_Loop_Part1"; exit 1; }
grep -q '^void Spc_Loop_Part2' "$O/nspc_player_gen.c" || { echo "SED MISS: Spc_Loop_Part2"; exit 1; }

# 1c) allocation goes through the wire: on the device the LLE Apu already
#     holds the AHB pool's 66 KB, so the player (66 KB more) must come from
#     the overlay BSS instead. wire.c decides; host maps it to malloc.
python3 - "$O/nspc_player_gen.c" <<'PYEOF'
import sys
p = sys.argv[1]
src = open(p).read()
old = """#ifdef TARGET_GNW
  /* 66 KB (64 KB of it is APU RAM). Take it from AHB RAM, not the overlay pool. */
  SpcPlayer *p = (SpcPlayer *)ahb_malloc(sizeof(SpcPlayer));
#else
  SpcPlayer *p = (SpcPlayer *)malloc(sizeof(SpcPlayer));
#endif"""
new = """  void *nspc_player_storage(void);
  SpcPlayer *p = (SpcPlayer *)nspc_player_storage();"""
assert src.count(old) == 1, "SpcPlayer_Create allocation anchor miss"
open(p, "w").write(src.replace(old, new))
print("allocation hook applied")
PYEOF

# 2) apu.c copy: original apu_run steps aside for wire.c's dispatcher
sed 's/^void apu_run(Apu\* apu, int cyclesToRun) {/void apu_run_lle(Apu* apu, int cyclesToRun) {/' \
    external/sm/src/snes/apu.c > "$O/apu_wire.c"
grep -q '^void apu_run_lle' "$O/apu_wire.c" || { echo "SED MISS: apu_run_lle"; exit 1; }

echo "gen_sources OK -> $O"
