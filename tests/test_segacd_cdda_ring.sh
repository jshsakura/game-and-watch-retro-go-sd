#!/bin/bash
# Compile the REAL CD-DA ring out of main_segacd.c and check it against the
# unpatched read sequence. usage: test_segacd_cdda_ring.sh [main_segacd.c]
set -eu
cd "$(dirname "$0")/.."
SRC=${1:-Core/Src/porting/segacd/main_segacd.c}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
awk '/^#define SEGACD_CDDA_RING_SECTORS/{on=1} /^\/\* common_emu_sound_sync\(false\)/{on=0} on' "$SRC" > "$T/ring_extract.inc"
[ -s "$T/ring_extract.inc" ] || { echo "FAIL: ring markers not found in $SRC"; exit 1; }
gcc -O1 -g -Wall -Wextra -Wno-unused-function -std=gnu11 -I"$T" tests/test_segacd_cdda_ring.c -o "$T/t" 2>&1 | grep -v 'unused' || true
"$T/t"
