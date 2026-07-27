#!/usr/bin/env python3
"""Downscale TPK2 sprite packs for the 320x240 Game & Watch screen.

The upstream packs are cut for a 466x466 panel (largest single action is
128x152 x11 frames = 209 KB). Our panel is 320x240, so the source detail is
paid for and never shown. This rescales every action in place, keeping the
TPK2 layout and the original RGB565 palette byte-for-byte, so the firmware
loader does not change at all.

Indices cannot be interpolated, so each frame is expanded to RGB through the
palette, resized, then mapped back to the nearest palette entry. Index 0xFF
(transparent) is carried through an alpha mask so edges do not bleed.

  python3 repack_tpk2.py <src_dir> <dst_dir> [scale]
"""
import os
import struct
import sys

from PIL import Image

TRANSPARENT = 0xFF
DEFAULT_SCALE = 0.5
MIN_DIM = 1


def rgb565_to_rgb888(c):
    r, g, b = (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def build_lut(pal565):
    """Palette as an RGB888 list plus a reverse map for nearest-entry lookup."""
    return [rgb565_to_rgb888(c) for c in pal565]


def nearest_index(rgb, pal888, cache):
    hit = cache.get(rgb)
    if hit is not None:
        return hit
    r, g, b = rgb
    best, best_d = 0, 1 << 30
    for i, (pr, pg, pb) in enumerate(pal888):
        d = (pr - r) ** 2 + (pg - g) ** 2 + (pb - b) ** 2
        if d < best_d:
            best, best_d = i, d
    cache[rgb] = best
    return best


def resize_frame(idx, w, h, nw, nh, pal888, cache):
    """Resize one indexed frame, preserving 0xFF as transparent."""
    rgb = Image.new('RGB', (w, h))
    alpha = Image.new('L', (w, h))
    rgb_px, a_px = [], []
    for v in idx:
        if v == TRANSPARENT or v >= len(pal888):
            rgb_px.append((0, 0, 0))
            a_px.append(0)
        else:
            rgb_px.append(pal888[v])
            a_px.append(255)
    rgb.putdata(rgb_px)
    alpha.putdata(a_px)

    rgb = rgb.resize((nw, nh), Image.LANCZOS)
    # BOX on the mask keeps the silhouette from growing a halo.
    alpha = alpha.resize((nw, nh), Image.BOX)

    out = bytearray(nw * nh)
    rgb_out, a_out = list(rgb.getdata()), list(alpha.getdata())
    for i, a in enumerate(a_out):
        out[i] = TRANSPARENT if a < 128 else nearest_index(rgb_out[i], pal888, cache)
    return bytes(out)


def repack(src, dst, scale):
    d = open(src, 'rb').read()
    if d[:4] != b'TPK2':
        raise ValueError('%s: not a TPK2 file' % src)

    o = 4
    n_acts = d[o]
    o += 1
    pal_count = struct.unpack_from('<H', d, o)[0]
    o += 2
    pal565 = list(struct.unpack_from('<%dH' % pal_count, d, o))
    o += pal_count * 2
    pal888 = build_lut(pal565)
    cache = {}

    out = bytearray(d[:o])  # header + palette carried over untouched
    for _ in range(n_acts):
        aid, w, h, n_frames = d[o], d[o + 1], d[o + 2], d[o + 3]
        o += 4
        ms = d[o:o + 2 * n_frames]
        o += 2 * n_frames

        nw = max(MIN_DIM, int(round(w * scale)))
        nh = max(MIN_DIM, int(round(h * scale)))
        frames = []
        for f in range(n_frames):
            frame = d[o:o + w * h]
            o += w * h
            frames.append(resize_frame(frame, w, h, nw, nh, pal888, cache))

        out += bytes([aid, nw, nh, n_frames]) + ms + b''.join(frames)

    open(dst, 'wb').write(out)
    return len(d), len(out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    scale = float(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_SCALE
    os.makedirs(dst_dir, exist_ok=True)

    names = sorted(f for f in os.listdir(src_dir) if f.endswith('.bin'))
    before = after = 0
    worst_before = worst_after = 0
    for name in names:
        src = os.path.join(src_dir, name)
        if open(src, 'rb').read(4) != b'TPK2':
            continue  # thumbs.bin and friends use other magics
        b, a = repack(src, os.path.join(dst_dir, name), scale)
        before += b
        after += a
        worst_before = max(worst_before, b)
        worst_after = max(worst_after, a)
        print('  %-12s %7d -> %7d' % (name, b, a))

    print('\nscale %.2f over %d files' % (scale, len(names)))
    print('  total  %.1f MB -> %.1f MB' % (before / 1048576, after / 1048576))
    print('  worst  %d B -> %d B' % (worst_before, worst_after))


if __name__ == '__main__':
    main()
