#!/usr/bin/env python3
"""Extract the nine flash-fallback sprites into sprites9.bin.

Upstream bakes small 32x32 character maps of the three starter lines into
species.h, used by the starter picker and by the evolution flash. They are
depictions of trademarked characters, so they live on the card rather than in
our binary -- the same arrangement as the species names, and for the same
reason: it is what lets the source be published.

The egg, poop and heart maps stay in the firmware. They are upstream's own
drawings of generic objects and depict nothing owned by anybody.

Format (little-endian):
  char[4] "TSPR"
  u8      count, u8 w, u8 h
  count x (h rows of w chars, no terminators)

  TAMAPOKE_UPSTREAM=/path/to/TamaPoke python3 make_fallback_sprites.py <out_dir>
"""
import os
import re
import struct
import sys

MAGIC = b'TSPR'
DIM = 32

# Order matters: it is the index the firmware's SPECIES table refers to.
WANTED = [
    'SPR_CHARMANDER', 'SPR_CHARMELEON', 'SPR_CHARIZARD',
    'SPR_BULBASAUR', 'SPR_IVYSAUR', 'SPR_VENUSAUR',
    'SPR_SQUIRTLE', 'SPR_WARTORTLE', 'SPR_BLASTOISE',
]


def read_sprite(src, name):
    m = re.search(r'static const char\* const %s\[%d\] = \{(.*?)\};' % (name, DIM),
                  src, re.S)
    if not m:
        raise SystemExit('%s not found in upstream species.h' % name)
    rows = re.findall(r'"([^"]*)"', m.group(1))
    if len(rows) != DIM:
        raise SystemExit('%s: expected %d rows, found %d' % (name, DIM, len(rows)))
    for r in rows:
        if len(r) != DIM:
            raise SystemExit('%s: row is %d chars, expected %d' % (name, len(r), DIM))
    return rows


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out_dir = sys.argv[1]
    upstream = os.environ.get('TAMAPOKE_UPSTREAM', '')
    path = os.path.join(upstream, 'species.h')
    if not os.path.exists(path):
        raise SystemExit(
            'cannot find %s\n'
            'set TAMAPOKE_UPSTREAM to a checkout of github.com/socquique/TamaPoke' % path)

    src = open(path, encoding='utf-8').read()
    blob = MAGIC + struct.pack('<3B', len(WANTED), DIM, DIM)
    for name in WANTED:
        for row in read_sprite(src, name):
            blob += row.encode('ascii')

    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, 'sprites9.bin')
    open(out, 'wb').write(blob)
    print('wrote %s: %d sprites, %d bytes' % (out, len(WANTED), len(blob)))


if __name__ == '__main__':
    main()
