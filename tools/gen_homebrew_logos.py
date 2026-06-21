#!/usr/bin/env python3
"""Generate the 1-bit `retro_logo_image` C structs for the Homebrew tab:

  - header_homebrew : "Homebrew" wordmark in DejaVu Serif Bold (replaces in place)
  - pad_homebrew    : a chunky beer-stein icon (homebrew = home-brewed beer pun)

Output format matches Core/Src/retro-go/rg_logos.c exactly:
width (multiple of 8), height, then row bytes MSB-first, with an ascii-art comment.
Run:  python3 tools/gen_homebrew_logos.py > /tmp/hb_structs.txt
"""
from PIL import Image, ImageDraw, ImageFont

SS = 6  # supersample for crisp 1-bit downscale

# "Homebrew" wordmark font: Sriracha (Cadson Demak, OFL) — casual brush/italic.
SERIF_BOLD = "tools/fonts/Sriracha-Regular.ttf"


def to_1bit(img_l, thresh):
    return img_l.point(lambda p: 255 if p >= thresh else 0).convert("1")


def pad_width_to_8(bit):
    """Right-pad the 1-bit image so width is a multiple of 8 (row stride safe)."""
    w, h = bit.size
    pw = (w + 7) // 8 * 8
    if pw == w:
        return bit
    out = Image.new("1", (pw, h), 0)
    out.paste(bit, (0, 0))
    return out


def emit_struct(name, bit):
    """Render a 1-bit PIL image as a rg_logos.c struct (string)."""
    w, h = bit.size
    assert w % 8 == 0, w
    px = bit.load()
    lines = [f"const retro_logo_image {name} LOGO_DATA = {{", f"    {w},", f"    {h},", "    {"]
    for y in range(h):
        row = []
        vis = ""
        for bx in range(0, w, 8):
            byte = 0
            for b in range(8):
                on = 1 if px[bx + b, y] else 0
                byte |= on << (7 - b)
                vis += "#" if on else "_"
            row.append(byte)
        hexs = ", ".join(f"0x{b:02x}" for b in row)
        left = f"        {hexs},"
        lines.append(f"{left}{' ' * max(1, 70 - len(left))} //  {vis}")
    lines += ["    },", "};"]
    return "\n".join(lines)


def render_wordmark(text, height):
    px = height * SS
    f = ImageFont.truetype(SERIF_BOLD, int(px * 1.32))
    tmp = Image.new("L", (px * 16, px * 3), 0)
    ImageDraw.Draw(tmp).text((4, 4), text, font=f, fill=255)
    crop = tmp.crop(tmp.getbbox())
    w = max(1, round(crop.width * height / crop.height))
    crop = crop.resize((w, height), Image.LANCZOS)
    return pad_width_to_8(to_1bit(crop, 110))


def render_pint(w, h, tilt_deg=20):
    """Filled pint glass with a foamy head + flying droplets, tilted `tilt_deg`
    to the left. Filled silhouette reads far better than line-art at ~48x32."""
    S = 360
    im = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(im)
    cx = S // 2
    top_y, bot_y = int(S * 0.40), int(S * 0.88)
    top_hw, bot_hw = int(S * 0.185), int(S * 0.15)
    # glass body, filled tapered
    d.polygon([(cx - top_hw, top_y), (cx + top_hw, top_y),
               (cx + bot_hw, bot_y), (cx - bot_hw, bot_y)], fill=255)
    # foam head, filled, overflowing (wider than the rim) with a bumpy top
    fhw = top_hw + int(S * 0.055)
    fy0, fy1 = int(S * 0.27), top_y + int(S * 0.02)
    d.rounded_rectangle([cx - fhw, fy0, cx + fhw, fy1], radius=int(S * 0.055), fill=255)
    for k in (-0.78, -0.3, 0.2, 0.68):
        bx, r = cx + int(fhw * k), int(S * 0.062)
        d.ellipse([bx - r, fy0 - r, bx + r, fy0 + r], fill=255)
    # negative beer line under the foam + bubbles, for a touch of detail
    d.line([(cx - top_hw + int(S*0.02), top_y + int(S*0.07)),
            (cx + top_hw - int(S*0.02), top_y + int(S*0.07))], fill=0, width=int(S*0.013))
    for bx, by, r in [(0.47, 0.63, 0.022), (0.545, 0.72, 0.017)]:
        rr = int(S * r)
        d.ellipse([int(S*bx)-rr, int(S*by)-rr, int(S*bx)+rr, int(S*by)+rr], fill=0)
    # flying foam droplets above the head
    for bx, by, r in [(0.30, 0.15, 0.024), (0.71, 0.12, 0.026), (0.81, 0.21, 0.017)]:
        rr = int(S * r)
        d.ellipse([int(S*bx)-rr, int(S*by)-rr, int(S*bx)+rr, int(S*by)+rr], fill=255)
    im = im.rotate(tilt_deg, resample=Image.BICUBIC, expand=False)   # CCW => top tilts left
    return pad_width_to_8(to_1bit(im.resize((w, h), Image.LANCZOS), 110))


if __name__ == "__main__":
    import sys

    word = render_wordmark("Homebrew", 24)
    stein = render_pint(48, 32)
    # also dump PNG previews for visual check
    if "--png" in sys.argv:
        word.resize((word.width * 3, word.height * 3)).save("/tmp/hb_word.png")
        stein.resize((stein.width * 6, stein.height * 6)).save("/tmp/hb_stein.png")
    print(f"// header_homebrew: {word.width}x{word.height}")
    print(emit_struct("header_homebrew", word))
    print()
    print(f"// pad_homebrew: {stein.width}x{stein.height}")
    print(emit_struct("pad_homebrew", stein))
