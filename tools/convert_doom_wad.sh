#!/usr/bin/env bash
# Convert a DOOM IWAD into the next-hack MCU format that the Game & Watch
# "nhdoom" engine expects on the SD card at /roms/homebrew/doom1.mcu.wad.
#
# WADs are never bundled with this firmware — supply your own. The freely
# redistributable DOOM shareware IWAD (DOOM1.WAD) works. Note the *converted*
# .mcu.wad is a modified derivative, so distribute only your own copy.
#
# Usage:
#   tools/convert_doom_wad.sh /path/to/DOOM1.WAD [output.mcu.wad]
# then copy the output onto the SD card as /roms/homebrew/doom1.mcu.wad
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONV="$HERE/external/nh-doom/MCUDoomWadUtil"   # converter source + gbadoom.wad
IN="${1:-}"
OUT="${2:-doom1.mcu.wad}"

if [ -z "$IN" ] || [ ! -f "$IN" ]; then
  echo "Usage: $0 <path/to/DOOM1.WAD> [output doom1.mcu.wad]" >&2
  echo "Then copy the output to the SD card at /roms/homebrew/doom1.mcu.wad" >&2
  exit 1
fi
if [ ! -f "$CONV/gbadoom.wad" ]; then
  echo "error: $CONV/gbadoom.wad missing (converter base WAD)" >&2
  exit 1
fi

# Build the converter on first use (plain host C).
BIN="$CONV/mcuwad"
if [ ! -x "$BIN" ]; then
  echo "Building MCUDoomWadUtil..."
  ( cd "$CONV" && cc -O2 -w main.c wadfile.c wadprocessor.c -o mcuwad )
fi

# MCUDoomWadUtil reads gbadoom.wad from its CWD, so run it from there with
# absolute in/out paths.
IN_ABS="$(cd "$(dirname "$IN")" && pwd)/$(basename "$IN")"
mkdir -p "$(dirname "$OUT")" 2>/dev/null || true
OUT_DIR="$(cd "$(dirname "$OUT")" 2>/dev/null && pwd || pwd)"
OUT_ABS="$OUT_DIR/$(basename "$OUT")"

( cd "$CONV" && ./mcuwad "$IN_ABS" "$OUT_ABS" )

echo
echo "Done: $OUT_ABS"
echo "Copy it onto the SD card as /roms/homebrew/doom1.mcu.wad"
