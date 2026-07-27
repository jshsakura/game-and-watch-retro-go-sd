#!/usr/bin/env python3
"""Tile the harness PPMs into a single contact-sheet PNG.

Usage:
    ./tools/tamapoke_harness/contact_sheet.py [screens_dir] [out.png]

Reads every *.ppm in screens_dir (default build/tamapoke_screens), sorts
them by filename, tiles into the smallest near-square grid that fits, and
writes a PNG. Requires Pillow.
"""
import os
import sys
from math import ceil, sqrt

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("Pillow is required: python3 -m pip install pillow\n")
    sys.exit(1)


def main():
    screens_dir = sys.argv[1] if len(sys.argv) > 1 else "build/tamapoke_screens"
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(screens_dir, "contact_sheet.png")

    paths = sorted(
        os.path.join(screens_dir, f)
        for f in os.listdir(screens_dir)
        if f.endswith(".ppm")
    )
    if not paths:
        sys.stderr.write(f"no PPMs in {screens_dir}\n")
        sys.exit(1)

    imgs = [Image.open(p) for p in paths]
    w, h = imgs[0].size

    cols = ceil(sqrt(len(imgs)))
    rows = ceil(len(imgs) / cols)
    pad = 2

    sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad) + pad), (32, 32, 32))
    for i, im in enumerate(imgs):
        r, c = divmod(i, cols)
        sheet.paste(im, (pad + c * (w + pad), pad + r * (h + pad)))

    sheet.save(out_path)
    print(f"wrote {out_path} ({cols}x{rows} grid, {len(imgs)} screens)")


if __name__ == "__main__":
    main()
