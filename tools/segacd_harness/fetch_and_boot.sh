#!/bin/bash
# Fetch Sega CD assets from the RPi5 fleet and run the host boot harness.
# Assets live on rpi-genie5 (Tailscale): /home/pi/app/ganda-rpi/data/library/public/
set -euo pipefail
DST=/tmp/scd; mkdir -p "$DST"
RP='rpi-genie5:/home/pi/app/ganda-rpi/data/library/public'
GAME="${1:-데토네이터 오건 (Detonator Orgun)}"
scp "$RP/_extra/bios/segacd/bios_CD_U.bin" "$DST/" 2>/dev/null || true
scp "$RP/roms/segacd/$GAME/"*.cue "$DST/" 2>/dev/null || true
scp "$RP/roms/segacd/$GAME/"*"Track 01"*.bin "$DST/" 2>/dev/null || true   # data track (boot code)
# audio tracks optional for initial boot; add them for CD-DA testing.
echo "fetched to $DST:"; ls -la "$DST"
# build + run from repo root:
#   (see boot_test build line in the segacd harness)
