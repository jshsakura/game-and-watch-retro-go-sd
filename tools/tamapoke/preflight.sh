#!/usr/bin/env bash
# Final check before pushing the TamaPoke branch and uploading the core.
#
# Everything here has failed at least once during this port, which is why each
# one is a check and not a note. Run it, read the summary, then push.
#
#   ./tools/tamapoke/preflight.sh [base_branch]     (default: testbed)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BASE="${1:-testbed}"
cd "$REPO"

pass=0; fail=0
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
note() { printf '       %s\n' "$1"; }

echo "== 1. nothing shareable-breaking in the tree =="

# The names were taken out of dex.h and species.h precisely so the source could
# be handed out. A literal creeping back in undoes that silently.
n=$(git grep -lE '"(BULBASAUR|CHARMANDER|SQUIRTLE|PIKACHU|MEWTWO)"' -- Core/ tools/ 2>/dev/null | wc -l)
[ "$n" -eq 0 ] && ok "no trademarked display strings" || bad "trademarked strings in $n file(s)"

# Art, not just names. The first version of this check only grepped for name
# strings and passed a tree that still held nine 32x32 maps of the starter
# lines -- a branch was pushed that way.
n=$(grep -rlE 'SPR_(CHARMANDER|CHARMELEON|CHARIZARD|BULBASAUR|IVYSAUR|VENUSAUR|SQUIRTLE|WARTORTLE|BLASTOISE)\[' Core/ 2>/dev/null | wc -l)
[ "$n" -eq 0 ] && ok "no trademarked sprite art" || bad "sprite art in $n file(s)"

# Sprite packs, the assets container, thumbnails: CC BY-NC, never committed.
n=$(git ls-files | grep -cE '\.(bin|dat|pak|img|ppm|tpk2)$' || true)
[ "$n" -eq 0 ] && ok "no asset binaries tracked" || bad "$n asset binary/binaries tracked"

# The PokeAPI cache holds the localised names verbatim. It was committed once.
n=$(git ls-files | grep -c 'pokeapi_cache' || true)
[ "$n" -eq 0 ] && ok "no PokeAPI cache tracked" || bad "PokeAPI cache tracked ($n files)"

echo
echo "== 2. the firmware carries no assets =="

# TamaPoke builds into every firmware now, like Zelda 3 and Super Mario World.
# That is only safe because the binary holds none of the material: names and
# starter sprites are read from the card at startup. If either ever gets baked
# back in, the release stops being publishable and nothing else would say so.
if [ -s build/gw_retro_go_intflash.bin ]; then
    if strings build/gw_retro_go_intflash.bin 2>/dev/null | grep -qiE '^(BULBASAUR|CHARMANDER|SQUIRTLE|PIKACHU|MEWTWO)$'; then
        bad "species names found in the firmware binary"
    else
        ok "no species names in the firmware binary"
    fi
    used=$(stat -c %s build/gw_retro_go_intflash.bin)
    # 256K bank. It has been at 99% since before this port; worth seeing.
    printf '       intflash %d/262144 B (%d%%), %d B free\n' \
        "$used" $(( used * 100 / 262144 )) $(( 262144 - used ))
    [ "$used" -le 262144 ] && ok "intflash fits" || bad "intflash overflow"
else
    note "no intflash image yet -- build first"
fi

echo
echo "== 3. the branch carries only this port =="

# The first attempt at this branch dragged four unrelated commits (an
# external/sm bump among them) toward a release. Only wiring may differ.
ALLOWED='scripts/update_gittag.sh|Core/Inc/gw_linker.h|Core/Src/retro-go/rg_emulators.c|Makefile|Makefile.common|STM32H7B0VBTx_SDCARD.ld|.gitignore|docs/TAMAPOKE'
stray=$(git diff --name-only "$BASE" 2>/dev/null | grep -v tamapoke | grep -vE "^($ALLOWED)" || true)
if [ -z "$stray" ]; then
    ok "diff vs $BASE is TamaPoke + wiring only"
else
    bad "unrelated files differ from $BASE:"
    echo "$stray" | sed 's/^/         /'
fi

echo
echo "== 4. layout and rendering =="
if ./tools/tamapoke_harness/run.sh >/tmp/tp_preflight_harness.log 2>&1; then
    ok "$(grep -oE '[0-9]+/[0-9]+ screens clean' /tmp/tp_preflight_harness.log) (both languages)"
else
    bad "harness failed -- see /tmp/tp_preflight_harness.log"
    grep -E '^FAIL|ERROR: AddressSanitizer' /tmp/tp_preflight_harness.log | head -5 | sed 's/^/         /'
fi

echo
echo "== 5. it links, and it fits =="
ELF=build/gw_retro_go.elf
MAP=build/gw_retro_go.map
if [ ! -s "$MAP" ]; then
    note "no map yet -- build with TAMAPOKE=1 first, skipping budget check"
else
    s=$(grep -aoE '0x[0-9a-f]+ +__ram_emu_tamapoke_start__' "$MAP" | grep -oE '0x[0-9a-f]+' | head -1)
    e=$(grep -aoE '0x[0-9a-f]+ +__ram_emu_tamapoke_end__' "$MAP" | grep -oE '0x[0-9a-f]+' | head -1)
    if [ -n "$s" ] && [ -n "$e" ]; then
        used=$(( e - s ))
        # __RAM_EMU_LENGTH__ = 1024K - 300K
        budget=741376
        pct=$(( used * 100 / budget ))
        if [ "$used" -lt "$budget" ]; then
            ok "RAM_EMU ${used}/${budget} B (${pct}%)"
        else
            bad "RAM_EMU overflow: ${used}/${budget} B"
        fi
    else
        note "overlay symbols absent from the map -- was TAMAPOKE=1 set?"
    fi
fi

# A core that references a global only another core defines gets silently bound
# to that core's address. The build runs this too; running it here means a
# failure is seen before the push rather than after.
if [ -s "$ELF" ] && [ -x scripts/check_core_symbol_aliases.py ]; then
    if python3 scripts/check_core_symbol_aliases.py "$ELF" >/tmp/tp_alias.log 2>&1; then
        ok "no cross-overlay symbol aliasing"
    else
        bad "cross-overlay aliasing -- see /tmp/tp_alias.log"
    fi
fi

echo
echo "== 6. nothing shareable-breaking in the release =="

# .gitignore does not protect the release: the tar is built from sd_content/,
# not from what git tracks. Assets staged there for testing rode into a
# published release once, which is how this check exists.
if [ -d sd_content/mons ] || [ -f sd_content/roms/homebrew/tamapoke_assets.dat ]; then
    bad "generated assets are in sd_content/ -- they would ship in the release tar"
else
    ok "no generated assets under sd_content/"
fi
if [ -s release/gw_update.tar ]; then
    n=$(tar tf release/gw_update.tar 2>/dev/null | grep -cE '^mons/|tamapoke_assets\.dat' || true)
    [ "$n" -eq 0 ] && ok "release tar carries no assets" || bad "release tar carries $n asset file(s)"
fi

echo
echo "== 7. the card payload =="
for f in sd_content/roms/homebrew/TamaPoke.bin; do
    [ -s "$f" ] && ok "$(basename "$f") $(stat -c %s "$f") B" || note "$f not built yet"
done
note "assets .dat is generated by stage_sd.sh from your own upstream checkout"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
