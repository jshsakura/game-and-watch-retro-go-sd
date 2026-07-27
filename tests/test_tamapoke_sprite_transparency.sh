#!/usr/bin/env bash
# The packer and the blit must agree on which palette index means "transparent".
#
# They did not. tools/tamapoke/repack_tpk2.py writes TRANSPARENT = 0xFF, and
# drawPmdActM() skipped index 0 instead -- so on hardware every transparent pixel
# was drawn as pal[255], which packs never fill (they carry a few dozen entries),
# and every pixel that legitimately used palette colour 0 was punched out of the
# sprite. The result was a black rectangle around the pet with a hollowed-out
# sprite inside it.
#
# Nothing could have caught that at compile time: the number lives in a Python
# generator and in C, and the two only meet inside a .dat file that is
# deliberately not in this tree (CC BY-NC sprites). So the check is textual, and
# cheap enough to run every time.
#
# It also asserts the blit bounds-checks the index against palCount. pal[] is 256
# entries but a pack fills only palCount of them, so drawing an out-of-range
# index reads uninitialised memory -- which is exactly what made the bug look
# like "transparency is not handled" rather than "the wrong number is skipped".
set -u
cd "$(dirname "$0")/.."
rc=0

PACKER="tools/tamapoke/repack_tpk2.py"
HEADER="Core/Inc/porting/tamapoke/tamapoke_sprites.h"
BLIT="Core/Src/porting/tamapoke/tamapoke_ui.cpp"

for f in "$PACKER" "$HEADER" "$BLIT"; do
    if [ ! -f "$f" ]; then
        echo "SKIP  $f is missing -- TamaPoke not in this tree"
        exit 0
    fi
done

echo "=== tamapoke: packer and blit agree on the transparent index ==="
# "TRANSPARENT = 0xFF" in the generator.
pack_val=$(sed -n 's/^TRANSPARENT[[:space:]]*=[[:space:]]*\(0[xX][0-9a-fA-F]\+\|[0-9]\+\).*/\1/p' "$PACKER" | head -1)
# "#define PMD_TRANSPARENT_INDEX 0xFF" in the firmware header.
fw_val=$(sed -n 's/^#define[[:space:]]\+PMD_TRANSPARENT_INDEX[[:space:]]\+\(0[xX][0-9a-fA-F]\+\|[0-9]\+\).*/\1/p' "$HEADER" | head -1)

if [ -z "$pack_val" ]; then
    echo "FAIL cannot find TRANSPARENT in $PACKER -- the gate has lost its subject"
    rc=1
elif [ -z "$fw_val" ]; then
    echo "FAIL cannot find PMD_TRANSPARENT_INDEX in $HEADER"
    rc=1
elif [ "$((pack_val))" -ne "$((fw_val))" ]; then
    echo "FAIL packer says $pack_val, firmware says $fw_val -- transparent pixels will"
    echo "     be drawn and opaque ones skipped, which reads as a solid box around"
    echo "     every sprite."
    rc=1
else
    printf 'OK  both say %d (%#x)\n' "$((pack_val))" "$((pack_val))"
fi

echo "=== tamapoke: the blit skips that index, not a hardcoded 0 ==="
if grep -q "idx == PMD_TRANSPARENT_INDEX" "$BLIT"; then
    echo "OK  drawPmdActM skips PMD_TRANSPARENT_INDEX"
else
    echo "FAIL the pack blit in $BLIT does not skip PMD_TRANSPARENT_INDEX."
    echo "     Whatever literal it skips instead is a second copy of this number."
    rc=1
fi

echo "=== tamapoke: the blit bounds-checks the palette index ==="
if grep -q "idx >= m.palCount" "$BLIT"; then
    echo "OK  an index past palCount is treated as transparent, not drawn"
else
    echo "FAIL the pack blit does not check idx against palCount -- pal[] has 256"
    echo "     entries and a pack fills only palCount of them, so a stray index"
    echo "     reads uninitialised memory and paints an arbitrary colour."
    rc=1
fi

exit $rc
