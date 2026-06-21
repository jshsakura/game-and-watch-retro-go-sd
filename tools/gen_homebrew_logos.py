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

SERIF_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"


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


def render_stein(w, h):
    """Chunky beer stein, filled silhouette with negative-space details.
    Designed to read at ~48x32 in a single colour."""
    W, H = w * SS, h * SS
    im = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(im)
    # body (the glass + beer)
    bx0, bx1 = int(W * 0.12), int(W * 0.60)
    by0, by1 = int(H * 0.34), int(H * 0.93)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=int(H * 0.10), fill=255)
    # foam dome on top, slightly overhanging the rim
    fy_top = int(H * 0.07)
    d.pieslice([bx0 - int(W * 0.04), fy_top, bx1 + int(W * 0.04), int(H * 0.50)], 180, 360, fill=255)
    d.rectangle([bx0 - int(W * 0.04), int(H * 0.28), bx1 + int(W * 0.04), int(H * 0.36)], fill=255)
    # thin gap separating foam from beer (negative space => reads as foam line)
    d.rectangle([bx0, by0 - int(H * 0.05), bx1, by0 - int(H * 0.015)], fill=0)
    # thick C-handle on the right with a negative hole
    hx0, hx1 = bx1 - int(W * 0.02), int(W * 0.93)
    hy0, hy1 = int(H * 0.45), int(H * 0.80)
    d.rounded_rectangle([hx0, hy0, hx1, hy1], radius=int(H * 0.14), fill=255)
    d.rounded_rectangle(
        [hx0 + int(W * 0.085), hy0 + int(H * 0.12), hx1 - int(W * 0.05), hy1 - int(H * 0.12)],
        radius=int(H * 0.08), fill=0,
    )
    # two rising foam bubbles (filled) above the dome for charm
    for cx, cy, r in [(0.30, 0.05, 0.045), (0.50, 0.02, 0.035)]:
        cxx, cyy, rr = int(W * cx), int(H * cy), int(H * r)
        d.ellipse([cxx - rr, cyy - rr, cxx + rr, cyy + rr], fill=255)
    # vertical shine stripe on the glass (negative)
    sx = int(W * 0.22)
    d.rectangle([sx, by0 + int(H * 0.12), sx + max(SS, int(W * 0.035)), by1 - int(H * 0.08)], fill=0)
    return pad_width_to_8(to_1bit(im.resize((w, h), Image.LANCZOS), 110))


if __name__ == "__main__":
    import sys

    word = render_wordmark("Homebrew", 24)
    stein = render_stein(48, 32)
    # also dump PNG previews for visual check
    if "--png" in sys.argv:
        word.resize((word.width * 3, word.height * 3)).save("/tmp/hb_word.png")
        stein.resize((stein.width * 6, stein.height * 6)).save("/tmp/hb_stein.png")
    print(f"// header_homebrew: {word.width}x{word.height}")
    print(emit_struct("header_homebrew", word))
    print()
    print(f"// pad_homebrew: {stein.width}x{stein.height}")
    print(emit_struct("pad_homebrew", stein))
