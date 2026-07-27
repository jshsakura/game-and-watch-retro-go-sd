#!/usr/bin/env bash
# Assemble the SD payload for the TamaPoke private build.
#
# ⚠️ The core is NOT droppable onto a stock firmware. The launcher dispatches
# homebrew by matching a name in resident code (rg_emulators.c), so the branch
# that calls app_main_tamapoke and the overlay in TamaPoke.bin must come from
# the SAME build. Copying only the .bin to an existing card does nothing: the
# entry will not appear. Flash the firmware from this build too.
#
# ⚠️ Sprites are CC BY-NC and depict Nintendo characters. They are generated
# locally from upstream's packs and must never be committed or published.
#
#   ./stage_sd.sh <upstream_mons_dir> <sd_root>
#
# <upstream_mons_dir> is TamaPoke/tools/sdcard/mons from a local checkout of
# https://github.com/socquique/TamaPoke, after running its tools/pack_pmd.py.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

SRC_MONS="${1:-}"
SD_ROOT="${2:-}"
SPRITE_SCALE="${SPRITE_SCALE:-0.5}"
THUMB_CELL="${THUMB_CELL:-24}"

if [ -z "$SRC_MONS" ] || [ -z "$SD_ROOT" ]; then
    sed -n '2,20p' "$0" >&2
    exit 2
fi
if [ ! -d "$SRC_MONS" ]; then
    echo "no such directory: $SRC_MONS" >&2
    exit 2
fi

CORE_BIN="$REPO/sd_content/roms/homebrew/TamaPoke.bin"
if [ ! -f "$CORE_BIN" ]; then
    echo "missing $CORE_BIN" >&2
    echo "build it first: make release DOCKER=1 TAMAPOKE=1 <the usual CI flags>" >&2
    exit 1
fi

STAGED_MONS="$REPO/build/tamapoke_mons"

# Upstream cuts sprites for a 466x466 panel: worst pack 495,503 B, and three can
# be live at once, which does not fit in 724 KB of RAM_EMU. At half scale the
# worst pack is 123,983 B and the whole set fits with room to spare. The
# firmware's static slots are sized to that, so this step is not optional.
echo "== rescaling sprite packs (${SPRITE_SCALE}x) =="
python3 "$HERE/repack_tpk2.py" "$SRC_MONS" "$STAGED_MONS" "$SPRITE_SCALE"

echo
echo "== regenerating gallery thumbnails (${THUMB_CELL}px) =="
python3 "$HERE/make_thumbs_320.py" "$STAGED_MONS" "$THUMB_CELL"

echo
echo "== packing species names =="
# The names are trademarks and are not in this tree; they come from the user's
# own upstream checkout and go straight to the card.
TAMAPOKE_UPSTREAM="${TAMAPOKE_UPSTREAM:-}" python3 "$HERE/make_dex_names.py" "$STAGED_MONS"
# Korean species names come from PokeAPI, not from us. A Korean UI showing
# BULBASAUR is not localised, and shipping the Korean list in our tree would
# undo the whole reason the names were taken out of it.
python3 "$HERE/make_dex_names.py" "$STAGED_MONS" ko

echo
echo "== packing the flash-fallback sprites =="
# The nine starter-line maps the starter picker draws. They used to be baked
# into the firmware; they are trademarked art, so they travel with the assets.
python3 "$HERE/make_fallback_sprites.py" "$STAGED_MONS"

echo
echo "== packing the assets sidecar =="
# One .dat rather than three hundred loose files: it is the shape Game & What
# already carries for homebrew (a .bin payload plus its assets .dat, as with
# smw_assets.dat and zelda3_assets.dat), and it keeps the firmware clear of the
# shared FatFS open-handle limit.
mkdir -p "$SD_ROOT/roms/homebrew"
python3 "$HERE/pack_assets_dat.py" "$STAGED_MONS" "$SD_ROOT/roms/homebrew/tamapoke_assets.dat"

echo
echo "== copying to $SD_ROOT =="
cp "$CORE_BIN" "$SD_ROOT/roms/homebrew/TamaPoke.bin"

WORST=$(find "$STAGED_MONS" -name 'p*.bin' -printf '%s\n' | sort -n | tail -1)
echo
echo "core   : $SD_ROOT/roms/homebrew/TamaPoke.bin"
echo "assets : $SD_ROOT/roms/homebrew/tamapoke_assets.dat (worst pack ${WORST} B)"
echo
echo "Now flash the firmware from THIS build as well -- the card alone is not enough."
