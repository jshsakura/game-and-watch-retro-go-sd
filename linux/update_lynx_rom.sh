#!/bin/bash
#
# Atari Lynx (Handy) HOST harness ROM fixture generator.
#
# There is no real .lyx ROM on disk, so this script deterministically builds a
# minimal *valid* LNX cartridge fixture (linux/lynx/test.lyx) and the
# compiled-in C array (linux/loaded_lynx_rom.c), mirroring update_a2600_rom.sh.
#
# Fixture layout (matches LYNX_HEADER in external/handy-go/cart.h, all LE):
#   off 0  magic[4]            = "LYNX"
#   off 4  page_size_bank0 u16 = 0x0100  -> CCart mMaskBank0 = 0x00FFFF (64K bank)
#   off 6  page_size_bank1 u16 = 0x0000  -> shadow SRAM/EEPROM bank (writable RAM)
#   off 8  version         u16 = 0x0001
#   off 10 cartname[32]        = "HARNESS TEST CART"
#   off 42 manufname[16]       = "HANDY"
#   off 58 rotation        u8  = 0  (CART_NO_ROTATE)
#   off 59 aud_bits        u8  = 0  (no Audin -> no bank0A path)
#   off 60 eeprom          u8  = 0
#   off 61 spare[3]            = 00 00 00
#   == 64-byte header ==
# Followed by BANK_BYTES of 0xFF cartridge data. We keep BANK_BYTES small
# (2048) so bank0size < mMaskBank0+1: CCart takes the RAM+0xFF-pad path and
# never reads past the file (no OOB). filesize >= 16 and magic=="LYNX" so
# CSystem::Init classifies it as HANDY_FILETYPE_LNX (0) = ACCEPTED.

set -e

cd "$(dirname "$0")"

LYXFILE=lynx/test.lyx
OUTFILE=loaded_lynx_rom.c
BANK_BYTES=2048

mkdir -p lynx

python3 - "$LYXFILE" "$BANK_BYTES" <<'PY'
import struct, sys
path, bank_bytes = sys.argv[1], int(sys.argv[2])
h  = b"LYNX"                              # magic[4]
h += struct.pack("<H", 0x0100)           # page_size_bank0
h += struct.pack("<H", 0x0000)           # page_size_bank1
h += struct.pack("<H", 0x0001)           # version
h += b"HARNESS TEST CART".ljust(32, b"\0")   # cartname[32]
h += b"HANDY".ljust(16, b"\0")               # manufname[16]
h += bytes([0])                          # rotation
h += bytes([0])                          # aud_bits
h += bytes([0])                          # eeprom
h += bytes([0, 0, 0])                    # spare[3]
assert len(h) == 64, len(h)
with open(path, "wb") as f:
    f.write(h)
    f.write(b"\xFF" * bank_bytes)
PY

SIZE=$(wc -c "$LYXFILE" | awk '{print $1}')

echo "const unsigned char ROM_DATA[] = {" > $OUTFILE
xxd -i < "$LYXFILE" >> $OUTFILE
echo "};" >> $OUTFILE
echo "unsigned int ROM_DATA_LENGTH = $SIZE;" >> $OUTFILE
echo "unsigned int cart_rom_len = $SIZE;" >> $OUTFILE
echo "const char *ROM_EXT = \"lyx\";" >> $OUTFILE

echo "Done! Wrote $LYXFILE ($SIZE bytes) and $OUTFILE"
