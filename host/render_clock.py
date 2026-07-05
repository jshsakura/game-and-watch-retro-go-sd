#!/usr/bin/env python3
# Turn /tmp/clockprev/*.bin (raw RGB565 320x240) into PNGs in host/preview_out/.
from PIL import Image
import glob, os, struct

W, H = 320, 240
os.makedirs("host/preview_out", exist_ok=True)
for path in sorted(glob.glob("/tmp/clockprev/*.bin")):
    name = os.path.splitext(os.path.basename(path))[0]
    data = open(path, "rb").read()
    img = Image.new("RGB", (W, H))
    px = img.load()
    for i in range(W * H):
        v = struct.unpack_from("<H", data, i * 2)[0]
        r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
        px[i % W, i // W] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    img = img.resize((W * 2, H * 2), Image.NEAREST)
    out = f"host/preview_out/clock_{name}.png"
    img.save(out)
    print("wrote", out)
