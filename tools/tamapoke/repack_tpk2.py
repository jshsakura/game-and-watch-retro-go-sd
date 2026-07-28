#!/usr/bin/env python3
"""Fit TPK2 sprite packs into the firmware's 124 KB sprite slot.

Upstream cuts these for a 466x466 panel. The firmware holds a pack in one of
three PMD_BLOB_MAX slots (tamapoke_sprites.h), so a pack has to fit 124 KB --
that, and nothing about the screen, is what forces any downscaling at all.

It used to downscale EVERYTHING to 0.5 unconditionally, which is why the pet came
out as an unrecognisable blob on hardware: a 32x40 sprite became 16x20, and at
that size the shape is gone. The size was never the problem for most packs --
p001 (Bulbasaur) is 31 KB at 0.5, a quarter of the slot it had to fit.

So the scale is per-pack now: keep the original resolution when the pack fits, and
step down only for the ones that do not (Onix and Gyarados are the big ones). Each
choice is printed, because a pack that had to be reduced is worth knowing about.

Where a downscale is unavoidable it happens on the INDICES, by majority vote per
destination block. The previous code expanded to RGB, resized with LANCZOS and
snapped back to the nearest palette entry -- for indexed art that is unsound in
principle, since an interpolated colour need not exist in the palette and the snap
then sends it anywhere. (Whether it actually damaged these particular sprites was
not established; the 0.5 scale is what did the visible damage.) A majority vote
cannot leave the palette, decides transparency by the same vote, and needs no PIL.

  python3 repack_tpk2.py <src_dir> <dst_dir> [scale]
"""
import os
import struct
import sys

TRANSPARENT = 0xFF
DEFAULT_SCALE = 1.0  # a ceiling: keep full resolution unless the slot says otherwise
MIN_DIM = 1


def resize_frame(idx, w, h, nw, nh):
    """Downscale one indexed frame by majority vote, never leaving the palette."""
    if (nw, nh) == (w, h):
        return bytes(idx)

    out = bytearray(nw * nh)
    for dy in range(nh):
        y0 = (dy * h) // nh
        y1 = max(y0 + 1, ((dy + 1) * h) // nh)
        for dx in range(nw):
            x0 = (dx * w) // nw
            x1 = max(x0 + 1, ((dx + 1) * w) // nw)

            counts = {}
            for sy in range(y0, y1):
                row = sy * w
                for sx in range(x0, x1):
                    v = idx[row + sx]
                    counts[v] = counts.get(v, 0) + 1

            # Most common index wins. Ties go to the lower index, which is
            # deterministic -- a tie broken by dict order would make the packer's
            # output depend on iteration order.
            best_v, best_n = TRANSPARENT, -1
            for v in sorted(counts):
                if counts[v] > best_n:
                    best_v, best_n = v, counts[v]
            out[dy * nw + dx] = best_v
    return bytes(out)


# tamapoke_sprites.h: PMD_BLOB_MAX. A pack larger than this is refused by the
# loader at runtime, so it is the only size that matters here.
# This repacker RESCALES and never DROPS: every action in the source pack comes out
# in the output, with the same action id. Worth stating, because the firmware's
# animation fallbacks look like they are covering for something this tool did.
# They are not -- measured over the 302 packs the container ships:
#
#     IDLE / WALKL / WALKR / SLEEP / HURT / ATTACK / HOP   302 of 302
#     POSE 58,  EAT 54,  NOD 52,  SIT 52,  BREATH 50
#
# So 82% of species have no Eat sprite AT SOURCE (SpriteCollab simply has none for
# them), which is why pickAct() in tamapoke_ui.cpp falls back to an action every
# pack carries. If a missing animation is ever reported, get the census from
# verify_assets_dat.py before looking in here.
SLOT_BYTES = 124 * 1024

# Tried in order; the first that fits is used. 1.0 first so a pack is only ever
# reduced because it has to be.
SCALE_LADDER = (1.0, 0.9, 0.8, 0.75, 0.7, 0.6, 0.5, 0.4, 0.35)


def pack_at_scale(d, scale):
    """Re-emit the whole pack at `scale`; returns the bytes."""
    o = 4
    n_acts = d[o]
    o += 1
    pal_count = struct.unpack_from('<H', d, o)[0]
    o += 2
    o += pal_count * 2  # the palette is carried through untouched

    out = bytearray(d[:o])
    for _ in range(n_acts):
        aid, w, h, n_frames = d[o], d[o + 1], d[o + 2], d[o + 3]
        o += 4
        ms = d[o:o + 2 * n_frames]
        o += 2 * n_frames

        nw = max(MIN_DIM, int(round(w * scale)))
        nh = max(MIN_DIM, int(round(h * scale)))
        frames = []
        for _f in range(n_frames):
            frame = d[o:o + w * h]
            o += w * h
            frames.append(resize_frame(frame, w, h, nw, nh))

        out += bytes([aid, nw, nh, n_frames]) + ms + b''.join(frames)
    return bytes(out)


def repack(src, dst, scale):
    """Write the largest-resolution pack that fits the slot. Returns
    (src_size, out_size, scale_used)."""
    d = open(src, 'rb').read()
    if d[:4] != b'TPK2':
        raise ValueError('%s: not a TPK2 file' % src)

    # `scale` from the command line is a ceiling, not an instruction: it caps how
    # large a pack may be kept, and the ladder walks down from there until the
    # result fits. Passing 1.0 (the default) means "as good as will fit".
    for candidate in SCALE_LADDER:
        if candidate > scale:
            continue
        out = pack_at_scale(d, candidate)
        if len(out) <= SLOT_BYTES:
            open(dst, 'wb').write(out)
            return len(d), len(out), candidate

    # Nothing on the ladder fit. Emit the smallest and say so loudly rather than
    # writing a pack the loader will refuse at runtime with no explanation.
    out = pack_at_scale(d, SCALE_LADDER[-1])
    open(dst, 'wb').write(out)
    print('  WARNING %s does not fit %d B even at %.2f (%d B) -- the loader will '
          'reject it' % (os.path.basename(src), SLOT_BYTES, SCALE_LADDER[-1], len(out)))
    return len(d), len(out), SCALE_LADDER[-1]


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
        b, a, used = repack(src, os.path.join(dst_dir, name), scale)
        before += b
        after += a
        worst_before = max(worst_before, b)
        worst_after = max(worst_after, a)
        print('  %-12s %7d -> %7d  scale %.2f%s'
              % (name, b, a, used, '' if used == 1.0 else '  (reduced to fit)'))

    print('\nscale %.2f over %d files' % (scale, len(names)))
    print('  total  %.1f MB -> %.1f MB' % (before / 1048576, after / 1048576))
    print('  worst  %d B -> %d B' % (worst_before, worst_after))


if __name__ == '__main__':
    main()
