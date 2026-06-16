#!/usr/bin/env python3
"""Read a 320x240 RGB565-LE framebuffer dump and report the color at each
quadrant center of the rendered cover, comparing against the expected pure
colors. A 240x240 square fit into 320x240 lands at x-offset 40, full height.
"""
import struct, sys

W, H = 320, 240
# image region after fit: 240x240 centered -> x in [40,280), y in [0,240)
OX, IS = 40, 240
SAMPLES = {
    "TL": (OX + IS // 4,     IS // 4,     (255, 0, 0),   "red"),
    "TR": (OX + 3 * IS // 4, IS // 4,     (0, 255, 0),   "green"),
    "BL": (OX + IS // 4,     3 * IS // 4, (0, 0, 255),   "blue"),
    "BR": (OX + 3 * IS // 4, 3 * IS // 4, (255, 255, 0), "yellow"),
}

def rgb565_to_rgb(v):
    r = ((v >> 11) & 0x1F) * 255 // 31
    g = ((v >> 5) & 0x3F) * 255 // 63
    b = (v & 0x1F) * 255 // 31
    return (r, g, b)

def classify(rgb):
    r, g, b = rgb
    hi = lambda v: v > 160
    lo = lambda v: v < 96
    if hi(r) and lo(g) and lo(b): return "red"
    if lo(r) and hi(g) and lo(b): return "green"
    if lo(r) and lo(g) and hi(b): return "blue"
    if hi(r) and hi(g) and lo(b): return "yellow"
    if hi(r) and lo(g) and hi(b): return "magenta"
    if lo(r) and hi(g) and hi(b): return "cyan"
    if lo(r) and lo(g) and lo(b): return "black"
    if hi(r) and hi(g) and hi(b): return "white"
    return "gray/other"

def main(path):
    with open(path, "rb") as f:
        data = f.read()
    def at(x, y):
        i = (y * W + x) * 2
        return rgb565_to_rgb(struct.unpack_from("<H", data, i)[0])
    print(f"== {path} ==")
    ok = True
    for name, (x, y, exp, expname) in SAMPLES.items():
        got = at(x, y)
        gotname = classify(got)
        mark = "OK " if gotname == expname else "XX "
        if gotname != expname: ok = False
        print(f"  {mark}{name}: expect {expname:7} got {gotname:11} rgb={got}")
    print("  => " + ("PASS" if ok else "MISMATCH"))
    return ok

if __name__ == "__main__":
    all_ok = True
    for p in sys.argv[1:]:
        all_ok &= main(p)
    sys.exit(0 if all_ok else 1)
