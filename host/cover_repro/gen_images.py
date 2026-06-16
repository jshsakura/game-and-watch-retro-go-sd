#!/usr/bin/env python3
"""Generate a reference cover with 4 distinct quadrants, in every format the
device decoder may hit. Quadrant colors are pure so any channel swap is obvious:
  TL=red(255,0,0)  TR=green(0,255,0)  BL=blue(0,0,255)  BR=yellow(255,255,0)
Each image is placed as a *sidecar* beside a fake .mp3 track so cover_load()'s
find_sidecar() picks it up (matching extension rules).
"""
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
IMG = os.path.join(HERE, "imgs")
os.makedirs(IMG, exist_ok=True)

S = 240
ref = Image.new("RGB", (S, S))
px = ref.load()
for y in range(S):
    for x in range(S):
        right = x >= S // 2
        bottom = y >= S // 2
        if not bottom and not right:   px[x, y] = (255, 0, 0)      # TL red
        elif not bottom and right:     px[x, y] = (0, 255, 0)      # TR green
        elif bottom and not right:     px[x, y] = (0, 0, 255)      # BL blue
        else:                          px[x, y] = (255, 255, 0)    # BR yellow

def track(name):
    # touch a fake mp3 so find_sidecar has a track to sit beside
    open(os.path.join(IMG, name + ".mp3"), "wb").close()
    return os.path.join(IMG, name)

# baseline JPEG
ref.save(track("jpgbase") + ".jpg", "JPEG", quality=92, progressive=False)
# progressive JPEG
ref.save(track("jpgprog") + ".jpg", "JPEG", quality=92, progressive=True)
# RGB PNG
ref.save(track("pngrgb") + ".png", "PNG")
# RGBA PNG
ref.convert("RGBA").save(track("pngrgba") + ".png", "PNG")
# palette PNG (indexed)
ref.convert("P", palette=Image.ADAPTIVE, colors=8).save(track("pngpal") + ".png", "PNG")
# grayscale PNG (expected: decoder skips, channels<3)
ref.convert("L").save(track("pnggray") + ".png", "PNG")

print("generated images in", IMG)
