#!/usr/bin/env python3
"""Render ALL console name-header wordmarks in the Luckiest Guy font (English)
and replace the existing header_* `retro_logo_image` structs in rg_logos.c
in place (same struct names => /bios/logo.bin order is preserved).

1-bit packing matches png_to_logo.py: rows padded to a multiple of 8 px,
MSB-first, visible pixel = 1.

  python3 tools/gen_name_logos.py <LuckiestGuy.ttf> --preview out.png        # preview only
  python3 tools/gen_name_logos.py <LuckiestGuy.ttf> --apply Core/Src/retro-go/rg_logos.c
"""
import argparse, re
from PIL import Image, ImageFont, ImageDraw

# struct name in rg_logos.c -> wordmark text. header_zelda3/header_smw are
# homebrew game banners, left untouched.
NAMES = {
    "header_nes":      "NES",
    "header_gb":       "GAMEBOY",
    "header_gbc":      "GAMEBOY COLOR",
    "header_lynx":     "ATARI LYNX",
    "header_gw":       "GAME & WATCH",
    "header_pce":      "PC ENGINE",
    "header_pcecd":    "PC ENGINE CD",
    "header_gg":       "GAME GEAR",
    "header_sms":      "MASTER SYSTEM",
    "header_gen":      "GENESIS",
    "header_sg1000":   "SG-1000",
    "header_col":      "COLECOVISION",
    "header_wsv":      "SUPERVISION",
    "header_ngp":      "NEOGEO POCKET",
    "header_wswan":    "WONDERSWAN",
    "header_msx":      "MSX",
    "header_a2600":    "ATARI 2600",
    "header_a7800":    "ATARI 7800",
    "header_amstrad":  "AMSTRAD CPC",
    "header_tama":     "TAMAGOTCHI",
    "header_pkmini":   "POKEMON MINI",
    "header_homebrew": "HOMEBREW",
    "header_pico8":    "PICO-8",
    "header_videopac": "ODYSSEY 2",
    "header_zx":       "ZX SPECTRUM",
    "header_c64":      "COMMODORE 64",
    "header_gamecom":  "GAME.COM",
    "header_favorites": "FAVORITES",
}

TARGET_H = 18      # matches existing header height
FONT_PX  = 22
THRESHOLD = 110


def render(font, text):
    pad = 8
    w = int(font.getlength(text)) + pad * 2
    tmp = Image.new("L", (w, FONT_PX * 3), 0)
    ImageDraw.Draw(tmp).text((pad, pad), text, font=font, fill=255)
    bbox = tmp.getbbox()
    g = tmp.crop(bbox)
    nw = max(1, round(g.width * TARGET_H / g.height))
    return g.resize((nw, TARGET_H), Image.Resampling.LANCZOS)


def pack(img):
    w, h = img.size
    pw = ((w + 7) // 8) * 8
    px = img.load()
    rows = []
    for y in range(h):
        row = []
        for xb in range(0, pw, 8):
            b = 0
            for bit in range(8):
                x = xb + bit
                if x < w and px[x, y] >= THRESHOLD:
                    b |= 1 << (7 - bit)
            row.append(b)
        rows.append(row)
    return pw, h, rows


def struct_text(name, pw, h, rows):
    out = [f"const retro_logo_image {name} LOGO_DATA = {{", f"    {pw},", f"    {h},", "    {"]
    for row in rows:
        hexv = ", ".join(f"0x{b:02x}" for b in row)
        vis = "".join("#" if (b >> (7 - i)) & 1 else "_" for b in row for i in range(8))
        out.append(f"        {hexv},  //  {vis}")
    out += ["    },", "};"]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("font")
    ap.add_argument("--preview")
    ap.add_argument("--apply")
    args = ap.parse_args()

    font = ImageFont.truetype(args.font, FONT_PX)
    rendered = {}
    for name, text in NAMES.items():
        g = render(font, text)
        pw, h, rows = pack(g)
        rendered[name] = (text, g, pw, h, rows)
        print(f"{name:18s} {text:16s} -> {pw}x{h}")

    if args.preview:
        gap = 6
        W = max(v[2] for v in rendered.values()) + 8
        H = sum(v[3] for v in rendered.values()) + gap * len(rendered) + 8
        c = Image.new("L", (W, H), 30)
        y = 4
        for name, (text, g, pw, h, rows) in rendered.items():
            c.paste(g.point(lambda v: 255 if v >= THRESHOLD else 0), (4, y))
            y += h + gap
        c.resize((W * 3, H * 3), Image.Resampling.NEAREST).save(args.preview)
        print(f"preview -> {args.preview}")

    if args.apply:
        src = open(args.apply).read()
        replaced, appended = 0, []
        for name, (text, g, pw, h, rows) in rendered.items():
            pat = re.compile(
                r"const retro_logo_image " + re.escape(name) + r" LOGO_DATA = \{.*?\n\};",
                re.DOTALL)
            new = struct_text(name, pw, h, rows)
            src, cnt = pat.subn(lambda m: new, src, count=1)
            if cnt == 1:
                replaced += 1
            else:
                appended.append(new)  # new header (e.g. header_lynx) -> append at end
        if appended:
            src = src.rstrip() + "\n\n/* New name headers (Luckiest Guy) */\n" + "\n\n".join(appended) + "\n"
        open(args.apply, "w").write(src)
        print(f"applied: {replaced} replaced, {len(appended)} appended -> {args.apply}")


if __name__ == "__main__":
    main()
