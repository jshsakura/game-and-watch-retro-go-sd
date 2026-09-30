#!/usr/bin/env bash
# Convert the CHD Sega CD library into cue/bin the device core streams.
#
#   segacd_chd_convert.sh [src_dir] [dst_dir]
#
# The device core (Core/Src/porting/segacd) reads .cue with raw 2352-byte
# BIN tracks -- data and audio alike (cd_image.c opens every non-MP3 track
# raw and ignores WAV headers). The launcher registers only the "cue"
# extension, so each game contributes exactly one browsable entry.
# On-device CHD decoding is not an option: libchdr wants ~150-200 KB of
# live RAM (map + rawmap + 3 codec buffers + cache/compare + LZMA state)
# and the segacd layout has ~30 KB of slack. chdman does the work offline.
#
# Output is one directory per game holding "<slug>.cue" plus
# "<slug> (Track NN).bin" files (~3x the CHD size). Idempotent: a game
# whose .cue already exists is skipped, so interrupted runs resume.
set -euo pipefail

SRC="${1:-/media/pi/EXTERNAL/miyoo-library/public/roms/segacd}"
DST="${2:-/media/pi/EXTERNAL/gnw-segacd-cue}"

slugify() {
  # keep the Korean title, drop the parenthesised English alias and ext
  local name="$1"
  name="${name%.chd}"
  name="${name// \(*}"
  echo "$name"
}

mkdir -p "$DST"
for chd in "$SRC"/*.chd; do
  name="$(basename "$chd")"
  slug="$(slugify "$name")"
  out="$DST/$slug"
  if [ -s "$out/$slug.cue" ]; then
    echo "SKIP  $slug (cue exists)"
    continue
  fi
  echo "CONV  $slug"
  mkdir -p "$out"
  chdman extractcd -f -i "$chd" -o "$out/$slug.cue" --splitbin >/dev/null
  echo "DONE  $slug ($(du -sh "$out" | cut -f1))"
done
echo "ALL DONE: $(ls "$DST" | wc -l) game dirs in $DST"
