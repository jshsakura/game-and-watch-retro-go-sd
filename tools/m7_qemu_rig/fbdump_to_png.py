#!/usr/bin/env python3
"""Rebuild a PNG from run_32x.sh's RIG_FB_DUMP output.

  EXTRA_DEF="-DRIG_FB_DUMP=200" bash tools/m7_qemu_rig/run_32x.sh rom.32x 300 > run.log
  python3 tools/m7_qemu_rig/fbdump_to_png.py run.log screen.png

A frozen 32X screen is nearly always a message, and a framebuffer checksum
cannot read it. This can.
"""
import re, sys, zlib, struct

def main(src, dst):
    px, w, h = [], 320, 240
    for line in open(src, encoding='utf-8', errors='replace'):
        m = re.match(r'\[fbd\] w?=?.*?w=(\d+) h=(\d+)', line)
        if m:
            w, h = int(m.group(1)), int(m.group(2)); continue
        m = re.match(r'\[fbd\] (\d+) ([0-9a-f]{4})\s*$', line)
        if m:
            px.extend([int(m.group(2), 16)] * int(m.group(1)))
    if not px:
        sys.exit("no [fbd] payload in " + src)
    px = (px + [0] * (w * h))[:w * h]

    rows = bytearray()
    for y in range(h):
        rows.append(0)                       # PNG filter: none
        for x in range(w):
            v = px[y * w + x]                # RGB565
            r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f
            rows += bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    with open(dst, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(rows), 9)))
        f.write(chunk(b'IEND', b''))
    print(f"{dst}: {w}x{h}, {len(set(px))} distinct colours")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'screen.png')
