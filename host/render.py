#!/usr/bin/env python3
# Convert raw RGB565 framebuffer dumps (/tmp/mpv/*.bin) to PNGs (3x for clarity).
import glob, os, struct
from PIL import Image

W, H, SCALE = 320, 240, 3
out_dir = "host/preview_out"
os.makedirs(out_dir, exist_ok=True)

for binf in sorted(glob.glob("/tmp/mpv/*.bin")):
    data = open(binf, "rb").read()
    img = Image.new("RGB", (W, H))
    px = img.load()
    for i in range(W * H):
        v = struct.unpack_from("<H", data, i * 2)[0]
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        px[i % W, i // W] = (r, g, b)
    name = os.path.splitext(os.path.basename(binf))[0]
    img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(f"{out_dir}/{name}.png")
    print(f"{out_dir}/{name}.png")
