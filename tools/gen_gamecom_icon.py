# Tiger game.com colour tab icon — 28x16, 4bpp, index 0 = transparent.
# Landscape unit: D-pad left, big LCD centre, 4 face buttons right,
# four small menu buttons under the screen (the real device's SOUND/PAUSE row).
ROWS = [
    "..bbbbbbbbbbbbbbbbbbbbbbbb..",
    ".bbaaaaaaaaaaaaaaaaaaaaaabb.",
    "baaaaddddddddddddddddaaaaaab",
    "baaadeeeeeeeeeeeeeedaa.h.aab",
    "baagdeeeeeeeeeeeeeeda.hhh.ab",
    "bagggdeeeeeeeeeeeeeeda.h.aab",
    "baagddeeeeeeeeeeeeeedaaaaaab",
    "baaadeeeeeeeeeeeeeeda.h.h.ab",
    "baaadeeeeeeeeeeeeeedaaaaaaab",
    "baaaaddddddddddddddddaaaaaab",
    "baaaaaaaaaaaaaaaaaaaaaaaaaab",
    "baaacacacacacaaaaaaaaaaaaaab",
    "baaaaaaaaaaaaaaaaaaaaaaaaaab",
    ".bcaaaaaaaaaaaaaaaaaaaaaacb.",
    "..cccccccccccccccccccccccc..",
    "...cccccccccccccccccccccc...",
]
IDX = {'.':0, 'a':1, 'b':2, 'c':3, 'd':4, 'e':5, 'f':6, 'g':7, 'h':8}
PAL = [
    0x0000,  # 0 transparent
    0x4A69,  # 1 body mid grey
    0x6B4D,  # 2 body highlight (top/left edge)
    0x2124,  # 3 body shadow (bottom)
    0x1082,  # 4 screen bezel, near black
    0x9E92,  # 5 LCD face, grey-green
    0x8410,  # 6 (spare) LCD shade
    0x18E3,  # 7 D-pad
    0x39E7,  # 8 buttons
] + [0x0000]*7

W, H = 28, 16
for i, r in enumerate(ROWS):
    assert len(r) == W, "row %d is %d chars, want %d" % (i, len(r), W)
assert len(ROWS) == H

px = [IDX[c] for r in ROWS for c in r]
data = []
for i in range(0, len(px), 2):
    data.append((px[i] << 4) | px[i+1])
assert len(data) == W*H//2 == 224

print("preview (background is dark):")
shade = {0:' ', 1:'#', 2:'+', 3:'.', 4:'@', 5:'-', 6:'=', 7:'X', 8:'o'}
for r in ROWS:
    print("   " + "".join(shade[IDX[c]] for c in r))

print()
print("static const uint16_t cicon_gamecom_pal[16] = {%s};" %
      ",".join("0x%04x" % c for c in PAL))
print("static const uint8_t cicon_gamecom_data[224] = {%s};" %
      ",".join("0x%02x" % b for b in data))
