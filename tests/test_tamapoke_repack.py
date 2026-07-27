#!/usr/bin/env python3
"""The sprite packer may never emit a palette index the source frame did not have.

This is the property the shipped packer broke. It expanded each indexed frame to
RGB, resized with LANCZOS, and snapped the result back to the nearest palette
entry -- so ringing at the edges invented colours, the snap sent them to whichever
entry was closest (often the black outline), and the transparent pixels, filled
with black before resizing, bled that black inward. What reached the device was a
formless blob: 71 opaque pixels in a 16x20 frame with 15 of them the darkest
palette entry, scattered through the middle of the body.

No firmware change could have fixed that -- the damage was already baked into
tamapoke_assets.dat -- and no rendering test could have seen it, because the
sprites are CC BY-NC and deliberately absent from this tree, so the host harness
draws the fallback art and never touches a pack.

What CAN be tested is the packer itself, on synthetic frames, against the rule
that makes indexed downscaling correct: every output index came from the block it
covers. Run: python3 tests/test_tamapoke_repack.py
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACKER = os.path.join(HERE, '..', 'tools', 'tamapoke', 'repack_tpk2.py')


def load_packer():
    if not os.path.exists(PACKER):
        print('SKIP  %s is missing -- TamaPoke not in this tree' % PACKER)
        sys.exit(0)
    spec = importlib.util.spec_from_file_location('repack_tpk2', PACKER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    m = load_packer()
    rc = 0

    def check(name, ok, detail=''):
        nonlocal rc
        if ok:
            print('OK  %s' % name)
        else:
            print('FAIL %s%s' % (name, ('  -- ' + detail) if detail else ''))
            rc = 1

    T = m.TRANSPARENT

    # A frame using a handful of indices, none of them the ones an interpolating
    # filter would invent between them. 4 and 9 are far apart on purpose: a
    # filter that averages produces something in between, and nothing in between
    # is allowed to appear in the output.
    w = h = 8
    src = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            src[y * w + x] = T if (x < 2 or y < 2) else (4 if (x + y) % 2 else 9)
    allowed = set(src)

    out = m.resize_frame(bytes(src), w, h, 4, 4)
    check('output is the requested size', len(out) == 16, 'got %d bytes' % len(out))
    stray = sorted(set(out) - allowed)
    check('no index appears that the source did not contain',
          not stray, 'invented %s; allowed %s' % (stray, sorted(allowed)))

    # Identity: same dimensions in, same bytes out. A resize that rewrites a
    # frame it was not asked to change is a second chance to corrupt it.
    same = m.resize_frame(bytes(src), w, h, w, h)
    check('a no-op resize returns the frame unchanged', same == bytes(src))

    # A block that is mostly transparent must stay transparent, and one that is
    # mostly opaque must not become transparent -- the silhouette is decided by
    # the same vote as the colour, so it cannot drift away from it.
    solid = bytes([7]) * (w * h)
    check('a fully opaque frame downscales to fully opaque',
          set(m.resize_frame(solid, w, h, 4, 4)) == {7})
    clear = bytes([T]) * (w * h)
    check('a fully transparent frame downscales to fully transparent',
          set(m.resize_frame(clear, w, h, 4, 4)) == {T})

    # Majority, not "whichever pixel happened to be sampled": in a 2x2 block
    # holding three 3s and one 5, the answer is 3.
    src2 = bytes([3, 3, 3, 5])
    check('a 2x2 block votes rather than samples',
          m.resize_frame(src2, 2, 2, 1, 1) == bytes([3]),
          'got %r' % m.resize_frame(src2, 2, 2, 1, 1))

    # Determinism: a tie must not depend on iteration order, or two runs of the
    # packer produce different .dat files from the same input.
    tie = bytes([6, 8, 8, 6])
    first = m.resize_frame(tie, 2, 2, 1, 1)
    check('a tie is broken deterministically',
          all(m.resize_frame(tie, 2, 2, 1, 1) == first for _ in range(5)))

    sys.exit(rc)


if __name__ == '__main__':
    main()
