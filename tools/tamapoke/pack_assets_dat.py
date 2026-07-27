#!/usr/bin/env python3
"""Pack the rescaled sprite packs, thumbnails and names into tamapoke_assets.dat.

Game & What already knows this shape: the homebrew system takes a ".bin" payload
plus a ".dat" assets sidecar that rides along into the SD ZIP, which is how
Super Mario World and Zelda 3 get their data (smw_assets.dat, zelda3_assets.dat).
Matching it means that project needs no changes to carry TamaPoke -- and the
card ends up with one file instead of three hundred, which also keeps us well
clear of the FatFS open-handle limit.

Container is upstream's own TPAK, so the layout is theirs, not a new invention:

  char[4]  "TPAK"
  u16      count
  count x  { u8 nameLen; char name[nameLen]; u32 size }
  ...blobs, in index order...

  python3 pack_assets_dat.py <staged_mons_dir> <out.dat>
"""
import os
import struct
import sys

MAGIC = b'TPAK'


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src_dir, out_path = sys.argv[1], sys.argv[2]

    if not os.path.isdir(src_dir):
        raise SystemExit('no such directory: %s' % src_dir)

    names = sorted(f for f in os.listdir(src_dir) if f.endswith('.bin'))
    if not names:
        raise SystemExit('nothing to pack in %s' % src_dir)

    blobs = [open(os.path.join(src_dir, n), 'rb').read() for n in names]

    index = struct.pack('<H', len(names))
    for name, blob in zip(names, blobs):
        encoded = name.encode('utf-8')
        if len(encoded) > 255:
            raise SystemExit('name too long for the index: %s' % name)
        index += struct.pack('<B', len(encoded)) + encoded + struct.pack('<I', len(blob))

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(MAGIC)
        f.write(index)
        for blob in blobs:
            f.write(blob)

    total = os.path.getsize(out_path)
    print('wrote %s: %d entries, %.1f MB' % (out_path, len(names), total / 1048576))


if __name__ == '__main__':
    main()
