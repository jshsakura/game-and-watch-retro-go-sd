# CPS-1 MAME Alignment — GFX/Video/Sound Spec, Confirmed

> Status: **confirmed against MAME source** (2026-07-21), not guessed. Every fact below
> was read directly out of `mamedev/mame` master (`src/mame/capcom/cps1.cpp`,
> `cps1.h`, `cps1_v.cpp`, `src/devices/sound/qsound{,hle}.{h,cpp}`), fetched with
> `curl` into this session and grepped/read verbatim — not summarized by a lossy
> web-fetch model (an earlier session in this initiative tried WebFetch/WebSearch for
> this exact question and got "does not contain" on files that plainly do; raw `curl`
> + local grep is what actually worked). File:line references below point at the
> fetched copies; re-fetch to reproduce.
>
> This supersedes every "UNCONFIRMED" bitplane/register-map placeholder left in
> `Core/Src/porting/cps1/cps1_rom.c`, `cps1_ppu.c`, `cps1_bg.c`, `cps1_core.c`, and
> `cps1_sound_hle.c` by the Phase 1–7 skeleton work. Section 8 lists exactly what
> changes where.

---

## 0. Critical correction first: the sound system

**`docs/CPS1_ULTIMATE_PORTING_PLAN.md` technique 6 and the Phase 6 skeleton
(`cps1_sound_hle.c`) targeted the wrong sound hardware for this game.**

`src/mame/capcom/cps1.cpp` GAME() table confirms:

```
GAME( 1992, wofj, wof, qsound, wof, cps_state, init_wof, ROT0, "Capcom",
      "Tenchi wo Kurau II: Sekiheki no Tatakai (Japan 921031)", ... )
```

`wofj` — Tenchi wo Kurau II, the exact target — uses the **`qsound`** machine config,
not the Z80+YM2151+OKI6295 config used by earlier CPS-1 titles (Final Fight, Ghouls'n
Ghosts, 1943 Kai, etc). QSound is Capcom's custom DSP16-based 16-voice PCM + 3-channel
ADPCM mixer with per-channel stereo panning and a shared FIR-filter/echo effects chain
— a materially different, and materially heavier, target than the phase-accumulator
square-wave HLE built in Phase 6. Section 6 below is the corrected spec; Phase 6's
`cps1_sound_hle.c` is not deleted (other, non-QSound CPS-1 titles still need it) but a
**new, separate `cps1_qsound_hle.c`** is what Tenchi wo Kurau II actually requires.

---

## 1. GFX ROM bitplane layout (CONFIRMED)

`cps1.cpp:3837-3886` (`GFXDECODE_START( gfx_cps1 )`):

```c
static const gfx_layout cps1_layout8x8 =
{
    8,8,
    RGN_FRAC(1,1),
    4,                          // planes
    { 24, 16, 8, 0 },           // planeoffset[plane] -- BIT offset, plane0=LSB..plane3=MSB
    { STEP8(0, 1) },            // xoffset[col] = col            (0,1,2,...,7)
    { STEP8(0, 4*16) },         // yoffset[row] = row*64          (0,64,128,...,448)
    64*8                        // total = 512 bits = 64 BYTES per tile
};
static const gfx_layout cps1_layout8x8_2 =    // right half of the SAME 16x16 block
{
    8,8, RGN_FRAC(1,1), 4,
    { 24, 16, 8, 0 },
    { STEP8(32, 1) },           // xoffset starts at bit 32 (byte 4), not bit 0
    { STEP8(0, 4*16) },
    64*8
};
static const gfx_layout cps1_layout16x16 =    // the whole 16x16 block, both halves
{
    16,16, RGN_FRAC(1,1), 4,
    { 24, 16, 8, 0 },
    { STEP8(0, 1), STEP8(32, 1) },   // left half then right half
    { STEP16(0, 4*16) },
    4*16*16                     // 128 bytes
};
static const gfx_layout cps1_layout32x32 =    // 4 vertical 8-wide strips
{
    32,32, RGN_FRAC(1,1), 4,
    { 24, 16, 8, 0 },
    { STEP8(0, 1), STEP8(32, 1), STEP8(64, 1), STEP8(96, 1) },
    { STEP32(0, 4*32) },
    4*32*32                     // 512 bytes
};
```

**Bit extraction** (MAME's standard convention, `devices/emu/gfxdecode` semantics): for
pixel (row, col) and plane p, the absolute bit index into the tile's byte block is

```
bitno = planeoffset[p] + yoffset[row] + xoffset[col]
bit   = (rom_byte[bitno / 8] >> (7 - bitno % 8)) & 1     // bit0 of bitno-space = MSB
pixel |= bit << p
```

**What this means physically, for an 8x8 tile (64 bytes = 8 bytes/row):**
- Each row occupies **8 consecutive bytes**, not 4. `yoffset[row] = row*64` (bits) =
  `row*8` (bytes).
- Within a row's 8 bytes, only **bytes 0–3** are `cps1_layout8x8` (the LEFT half-tile);
  bytes 4–7 are `cps1_layout8x8_2` (the RIGHT half-tile) — `xoffset` starts at bit 32 =
  byte 4. **The ROM's physical atomic unit is a 16x16 4bpp block (128 bytes); CPS-1's
  "8x8" SCROLL1 tiles are the left or right half of one of these blocks**, selected by
  `BIT(tile_index, 5)` in `get_tile0_info` (`cps1_v.cpp:2461`) — i.e. bit 5 of the
  *tilemap column index* (not the tile code), so it alternates by screen column, not by
  tile content. This is the exact behavior the source comment at `cps1.cpp:3833` warns
  about ("columns of the 8x8 tilemap alternate between sides of the 16x16 tile").
- Within one half-tile's 4 bytes (one row), `planeoffset={24,16,8,0}` (bits) =
  `{3,2,1,0}` (byte-within-half-row): **byte 3 holds plane 0 (LSB of the 4bpp pixel
  index), byte 2 holds plane 1, byte 1 holds plane 2, byte 0 holds plane 3 (MSB)**.
- `xoffset[col]=col` and MSB-first bit extraction means **column 0 = bit 7 of each
  plane byte, column 7 = bit 0** — i.e. bit-reversed relative to a naive "bit N = pixel
  N" reading. `Core/Src/porting/cps1/cps1_rom.c`'s existing `cps1_rom_decode_tile_planar`
  already extracts MSB-first (`bit_pos = 7 - col`), which happens to match — that part
  needs no change, only the `CPS1_GFX_LAYOUT_DEFAULT` constants do (see §8).
- 16x16 (SCROLL2) = the whole 128-byte block, both halves, used whole.
- 32x32 (SCROLL3) = **four vertically-adjacent 8-pixel-wide × 32-row-tall strips**
  (`xoffset` groups at byte 0/4/8/12, `yoffset` steps 128 bits = 16 bytes/row over 32
  rows), **not** a 4×4 grid of 8x8 sub-tiles as the Phase 5 skeleton assumed. Both are
  512 bytes total either way, but the byte-to-pixel mapping differs — matters if ever
  loading a byte-exact real ROM dump, not just synthetic test data.

Tile-code addressing unit: all four gfx views share ONE underlying byte stream, just
sliced differently, so a "tile code" means different things depending on which layer
uses it — see `gfxrom_bank_mapper`'s per-type `shift` in §5.

---

## 2. Palette format (CONFIRMED) — 12-bit RGB + 4-bit brightness, NOT RGB565

`cps1_v.cpp:2612-2643` (`cps1_build_palette`):

```c
const uint16_t palette = *(palette_ram++);           // raw 16-bit word from gfxram
const int bright = 0x0f + ((palette >> 12) << 1);    // brightness nibble -> 0x0f..0x2d
const int r = ((palette >> 8) & 0x0f) * 0x11 * bright / 0x2d;
const int g = ((palette >> 4) & 0x0f) * 0x11 * bright / 0x2d;
const int b = ((palette >> 0) & 0x0f) * 0x11 * bright / 0x2d;
```

Raw word layout: `bbbb-rrrr-gggg-bbbb`... precisely: bits 15-12 = brightness, bits
11-8 = R, bits 7-4 = G, bits 3-0 = B (4 bits each). `* 0x11` expands a nibble (0-15) to
a byte (0-255) the same way `0xF*0x11=0xFF` does; brightness of 0 doesn't mean black,
it scales to 1/3 (`bright=0x0f`, `bright/0x2d = 15/45 = 1/3`) per the comment — a
deliberate hardware quirk (used for fades), not a bug.

Palette RAM is **6 pages of 0x200 (512) entries** = 32 sub-palettes × 16 colors per
page, copied from `gfxram` (at the address in `CPS1_PALETTE_BASE`) to the device's
real palette RAM **only for pages whose bit is set** in the CPS-B `palette_control`
register (`cps1_v.cpp:2618`, `m_game_config->palette_control` — for `wof`,
offset `0x2c`, see §4). Skipped pages before the first copied one don't advance the
read pointer; skipped pages after do (`cps1_v.cpp:2635-2642`) — an easy-to-miss
off-by-one-page trap if re-implemented from scratch.

**Device implication**: RGB565 conversion must happen at *build_palette* time (once
per palette-base write, not per-pixel) — precompute a 256-entry (or per-page) RGB565
LUT the way MAME precomputes into `palette_device`, not a live per-pixel nibble
expansion. `cps1_ppu.h`'s `cps1_palette_t` (currently `uint16_t colors[32][16]`
holding already-RGB565 test values) is the right *shape* — 32 banks × 16 colors per
bank matches "32 sub-palettes × 16 colors per page" exactly — but the skeleton never
implements the raw-word → RGB565 conversion; that function doesn't exist yet.

---

## 3. Memory map (CONFIRMED, `cps1.cpp` `qsound_main_map`, used by `wof`)

```
0x000000-0x1FFFFF   PRG ROM
0x800000-0x800007   IN1 (player input)
0x800018-0x80001F   DSW / system inputs
0x800030-0x800037   coin control
0x800100-0x80013F   CPS-A registers (write-only)      <- see §4
0x800140-0x80017F   CPS-B registers (read/write)       <- see §4
0x900000-0x92FFFF   gfxram (192KB, RAM+write handler)  <- OBJ/SCROLL1/2/3/PALETTE/OTHER
                    ALL live inside this one pool, at
                    RELOCATABLE offsets set by CPS-A base regs (§4) -- there is
                    no fixed hardwired address per region.
0xF00000-0xF0FFFF   qsound_rom_r (Slammasters-protection passthrough; ignore for wof)
0xF18000-0xF19FFF   QSound shared RAM window 0 (8KB, mirrors Z80-side 0xC000-0xCFFF)
0xF1C000-0xF1C001   IN2 (3rd player, later games)
0xF1C002-0xF1C003   IN3 (4th player)
0xF1C004-0xF1C005   coin control 2
0xF1C006-0xF1C007   EEPROM in/out
0xF1E000-0xF1FFFF   QSound shared RAM window 1 (8KB, mirrors Z80-side 0xF000-0xFFFF)
0xFF0000-0xFFFFFF   mainram (68000 WRAM)
```

**This is architecturally different from the Phase 1-7 skeleton's bus**, which
hardwired fixed addresses for WRAM/OBJ/palette/BG-tilemap/sound-cmd
(`CPS1_WRAM_BASE=0xFF0000`, `CPS1_OBJ_BASE=0x900000`, etc. in `cps1_core.c`). Real
CPS-1 has exactly ONE big shared RAM pool (`gfxram`, 0x900000-0x92FFFF) and the CPS-A
registers *contain the current offset* of each logical region within it (relocatable,
double-buffer-friendly) — not fixed per-region addresses. `CPS1_WRAM_BASE=0xFF0000`
happens to be right (that's real `mainram`); the OBJ/palette/BG addresses are not.

---

## 4. CPS-A / CPS-B registers (CONFIRMED, `cps1.h:172-193`)

CPS-A (base 0x800100, write-only, word offsets ÷2 already applied below):

```
0x00  CPS1_OBJ_BASE          -- gfxram offset (×256) of OBJ RAM
0x02  CPS1_SCROLL1_BASE      -- gfxram offset of SCROLL1 tilemap
0x04  CPS1_SCROLL2_BASE      -- gfxram offset of SCROLL2 tilemap
0x06  CPS1_SCROLL3_BASE      -- gfxram offset of SCROLL3 tilemap
0x08  CPS1_OTHER_BASE        -- gfxram offset of "other" video RAM (row-scroll table)
0x0A  CPS1_PALETTE_BASE      -- gfxram offset of palette data (writing this ALSO
                                 triggers an immediate copy into real palette RAM --
                                 cps1_v.cpp:2119-2126)
0x0C  CPS1_SCROLL1_SCROLLX   0x0E  CPS1_SCROLL1_SCROLLY
0x10  CPS1_SCROLL2_SCROLLX   0x12  CPS1_SCROLL2_SCROLLY
0x14  CPS1_SCROLL3_SCROLLX   0x16  CPS1_SCROLL3_SCROLLY
0x18  CPS1_STARS1_SCROLLX    0x1A  CPS1_STARS1_SCROLLY
0x1C  CPS1_STARS2_SCROLLX    0x1E  CPS1_STARS2_SCROLLY
0x20  CPS1_ROWSCROLL_OFFS    -- base of row-scroll table, in OTHER ram
0x22  CPS1_VIDEOCONTROL      -- flip screen, rowscroll enable (bits 2/3 also gate
                                 SCROLL2/SCROLL3 tilemap enable, cps1_v.cpp:2317-2318)
```

Base-address registers are **byte offset ÷ 256** (`cps1_v.cpp:2099`:
`base = m_cps_a_regs[offset] * 256`), then masked to a boundary
(`m_scroll_size`/`m_obj_size`, game-dependent, typically 0x4000) and clamped into the
0x900000-0x92FFFF pool (`& 0x3ffff`).

CPS-B (base 0x800140) is **per-game-config**, not fixed — `cps1.h`'s `CPS1config`
struct holds every offset as a field, and each game/PCB variant supplies its own
values via a macro. For `wof`/`wofj` (`CPS_B_21_QS1`, `cps1_v.cpp:507`):

```
layer_control     = 0x22
priority[0..3]    = 0x24, 0x26, 0x28, 0x2a      // 4 sprite-priority bitmask registers
palette_control   = 0x2c                          // per-page palette-copy enable (§2)
layer_enable_mask = { 0x10, 0x08, 0x04, 0x00, 0x00 }
                    // bit tested in layer_control for: SCROLL1, SCROLL2, SCROLL3,
                    // stars1(unused=0), stars2(unused=0)
mult_factor1/2, mult_result_lo/hi, unknown1/2/3, cpsb_addr/value = -1 (unused --
    wof has no board-B self-test/multiply-protection quirk to emulate)
```

Sprite priority is a **4-entry bitmask table** (`priority[0..3]`), not a single
2-bit-per-tile scheme — each entry is a 16-bit mask tested against the sprite's own
priority bits to decide which BG-layer priority group it draws above/below
(`cps1_render_sprites`, not fully quoted here — flagged as a §7 follow-up, this is the
most complex remaining piece and genuinely deserves its own read-through before
implementing).

Sound command latch (`cps1_soundlatch_w`/`_w2`, `cps1.cpp:302-313`, mapped at
`0x800180-0x800187`/`0x800188-0x80018f` in the *non-QSound* configs) is **not used by
`wof`** — QSound games route sound entirely through the shared-RAM windows (§3, §6),
bypassing the simple soundlatch mechanism the Phase 6 skeleton (`cps1_sound_hle.c`,
`CPS1_SOUND_CMD_BASE`) was built against.

---

## 5. OBJ (sprite) table (CONFIRMED, `cps1_v.cpp:2649-2668`)

```
xx xx yy yy nn nn aa aa     (4 words per sprite: X, Y, tile number, attribute)

attribute word:
  bits 0-4    color (0-31)
  bit  5      X flip
  bit  6      Y flip
  bit  7      X/Y offset toggle (Marvel vs. Capcom uses this)
  bits 8-11   X block size (multi-tile sprites, in units of 16x16 tiles)
  bits 12-15  Y block size

End-of-table marker: attribute word == 0xff00 (find_last_sprite scans for it,
cps1_v.cpp:2684).
```

**Word order is X,Y,tile,attr** — the Phase 5 skeleton's `cps1_oam_entry_t`
(`Y,tile,attr,X`, per the `cps1_core.c` bus dispatch's `case 0:y / case1:tile /
case2:attr / default:x`) has the wrong order and is missing multi-tile block-size
sprites entirely (every real CPS-1 sprite can be an NxM grid of 16x16 tiles, not just
one).

**Sprites are delayed one whole frame** (`cps1_v.cpp:3063-3069`,
`cps1_objram_latch`): the CPU's writes to OBJ RAM this frame are only copied into the
buffer the renderer actually reads (`m_buffered_obj`) at a specific vblank-adjacent
timing point, so what's drawn is always last frame's sprite data. **This is a
real hardware behavior a byte-correct port must reproduce** (getting it wrong causes
subtle one-frame-early sprite-position glitches, not a crash) — the Phase 5/7
skeleton renders sprites from the live OAM the same frame they're written, with no
buffering, and needs this added.

---

## 6. SCROLL (BG) tilemap format (CONFIRMED, `cps1_v.cpp:2434-2507`)

All three layers are **64x64-cell tilemaps** regardless of tile size (SCROLL1: 8x8
tiles → 512x512px; SCROLL2: 16x16 → 1024x1024px; SCROLL3: 32x32 → 2048x2048px) — the
Phase 5 skeleton's `CPS1_BG_MAP_W/H = 64` was, happily, already correct.

**Cell format: 2 words per cell**, `code` then `attr`:

```c
int code = m_scroll[layer][2*tile_index];
uint16_t attr = m_scroll[layer][2*tile_index + 1];
code = gfxrom_bank_mapper(GFXTYPE_SCROLLn, code);   // §5-of-this-doc, i.e. next section

tileinfo.set(gfxset, code, (attr & 0x1f) + layer_palette_offset,
             TILE_FLIPYX((attr & 0x60) >> 5));
tileinfo.group = (attr & 0x0180) >> 7;               // 2-bit priority group (0-3)
```

- `layer_palette_offset`: **SCROLL1 = +0x20, SCROLL2 = +0x40, SCROLL3 = +0x60**
  (`cps1_v.cpp:2464/2484/2502`) — confirms sprite palettes occupy banks 0x00-0x1F,
  SCROLL1 banks 0x20-0x3F, SCROLL2 0x40-0x5F, SCROLL3 0x60-0x7F (matches
  `GFXDECODE_ENTRY(...,0,0x80)`'s 0x80 = 128 total color groups).
- attr bit 5 = X flip, bit 6 = Y flip (`TILE_FLIPYX((attr&0x60)>>5)`).
- attr bits 7-8 = **priority group** (2 bits, 4 groups per layer) — this is the actual
  "software Z-buffer" data cheat 8 wants to avoid computing per-pixel. MAME resolves
  it via tilemap "groups" + transparency-mask draws (`cps1_render_layer`/
  `cps1_update_transmasks`), not a single fixed layer-order. **The Phase 5 skeleton's
  compositor (`cps1_compositor_blend`: bottom always shows, middle/top overwrite
  unconditionally) has NO priority-group handling at all** — it's a strict
  bottom<middle<top layer order with no intra-layer or sprite-vs-layer priority. This
  is the single biggest correctness gap for a real port and deserves its own
  implementation phase (§7), not a quick patch.
- SCROLL1's `gfxset` (which of the two 8x8 GFXDECODE entries to use) is
  `BIT(tile_index, 5)` — bit 5 of the tilemap *index* (screen column parity via the
  scan function below), not of the tile code.

**Tilemap addressing is bit-swizzled, not row-major** (`cps1_v.cpp:2434-2448`):

```c
// SCROLL1 (8x8): logical (col,row) -> memory offset
offset = (row & 0x1f) + ((col & 0x3f) << 5) + ((row & 0x20) << 6);
// SCROLL2 (16x16):
offset = (row & 0x0f) + ((col & 0x3f) << 4) + ((row & 0x30) << 6);
// SCROLL3 (32x32):
offset = (row & 0x07) + ((col & 0x3f) << 3) + ((row & 0x38) << 6);
```

This only matters for byte-exact compatibility with a real VRAM dump/savestate; it
does not matter for the synthetic test data the skeleton currently uses (which can
pick any addressing convention as long as it's internally consistent), but **must be
implemented exactly this way once real ROM/VRAM data is involved**, or every tile
beyond row 31/15/7 (SCROLL1/2/3 respectively) reads from the wrong cell.

---

## 7. GFX ROM bank mapping — confirmed for `wof` specifically (`cps1_v.cpp:2385-2420`)

```c
int cps_state::gfxrom_bank_mapper(int type, int code)
{
    shift = (type==SPRITES) ? 1 : (type==SCROLL1) ? 0 : (type==SCROLL2) ? 1 : 3; // SCROLL3
    code <<= shift;
    // find the gfx_range whose [start,end] contains `code`, whose `type` bitmask
    // includes ours, add up bank_sizes[0..range.bank-1], mask code into that bank,
    // shift back down.
}
```

For `wof`/`wofj` specifically (`{"wof", CPS_B_21_QS1, mapper_TK263B}`,
`cps1_v.cpp:1975`, table at `cps1_v.cpp:1373-1384`):

```c
#define mapper_TK263B   { 0x8000, 0x8000, 0, 0 }, mapper_TK263B_table   // bank_sizes
static const struct gfx_range mapper_TK263B_table[] = {
    { SPRITES|SCROLL1|SCROLL2|SCROLL3, 0x00000, 0x07fff, 0 },   // bank 0, 32KB
    { SPRITES|SCROLL1|SCROLL2|SCROLL3, 0x08000, 0x0ffff, 1 },   // bank 1, 32KB
    { 0 }
};
```

**Good news for this specific game**: unlike many other CPS-1 titles (which split
sprites/SCROLL1/2/3 into separate ROM-bank ranges — see `mapper_TK22B_table` a few
lines above `TK263B` for a contrasting example with 4 disjoint ranges), `wof`'s
bank mapper treats **all four gfx types as one unified 64KB "8x8-tile-code" address
space**, split at the midpoint into two 32KB physical banks. Once the two ROM chip
groups are byte-interleaved into one flat region at load time (§8, already implemented
as `cps1_rom_load_interleaved`), **the bank split is fully transparent** — a
tile/sprite code (after the type-dependent `<<shift`) indexes directly and linearly
into the flat combined GFX blob. This is simpler to implement than the general case
and should be the first target.

---

## 8. What changes in the existing skeleton, concretely

| File | Current (Phase 1-7 skeleton) | Confirmed real value | Action |
|---|---|---|---|
| `cps1_rom.h` `CPS1_TILE_SIZE_BYTES` | 32 | **64** (§1) | change constant, re-check every caller |
| `cps1_rom.c` `CPS1_GFX_LAYOUT_DEFAULT` | planes=4, offsets `{0,8,16,24}`, 1 byte/row/plane | planes=4, `planeoffset={24,16,8,0}` (bit units), yoffset step 64 bits/row, **and a required second "layout8x8_2" (right-half) variant + the 16x16/32x32 shapes from §1** | replace with §1's exact layout; `cps1_rom_decode_tile_planar`'s MSB-first bit read is already correct, keep it |
| `cps1_ppu.h` `cps1_oam_entry_t` | `x,y,tile_index,attr` (4 separate fields, no block size) | word order **X,Y,tile,attr**; attr bits 0-4 color/5 xflip/6 yflip/7 offset-toggle/8-11 Xblock/12-15 Yblock (§5) | add block-size (multi-tile) rendering; fix bus word order in `cps1_core.c` |
| `cps1_ppu.c` / `cps1_core.c` | sprites render same-frame from live OAM | **one-frame delayed** via a buffered copy latched at vblank (§5) | add a `cps1_oam_t m_buffered_obj` + a latch step in the frame loop, render from the buffer not the live table |
| `cps1_bg.h` `cps1_bg_cell_t` | `tile_index, palette, enabled` (no priority, no flip) | `code, attr` 2-word cell; attr bits 0-4 color(+layer offset 0x20/0x40/0x60)/5 xflip/6 yflip/7-8 priority-group (§6) | rework cell struct + bus decode; flip not implemented at all yet |
| `cps1_bg.c` `cps1_compositor_blend` | strict bottom<middle<top, no priority | needs the 2-bit priority-group + sprite-priority-bitmask system (§4, §6) | **biggest remaining gap** — needs its own design pass, not a patch; MAME's transmask approach is the reference |
| `cps1_ppu.h` `cps1_palette_t` | `uint16_t[32][16]`, values written as raw RGB565 | raw words are **12-bit RGB + 4-bit brightness** (§2); shape (32×16) is already right | add the raw-word → RGB565 conversion function per §2's exact formula, call it from the palette-base-write path, not per-pixel |
| `cps1_core.c` bus (`CPS1_WRAM_BASE`, `CPS1_OBJ_BASE`, `CPS1_PAL_BASE`, `CPS1_BG_BASE`, `CPS1_SOUND_CMD_BASE`) | 5 independent fixed regions | **one shared 192KB `gfxram` pool (0x900000-0x92FFFF) + relocatable CPS-A base registers (0x800100-0x80013F) pointing into it** (§3, §4); `CPS1_WRAM_BASE=0xFF0000` is correct as-is | replace OBJ/palette/BG addressing with the indirect base-register scheme; keep WRAM address |
| `cps1_sound_hle.c` | phase-accumulator tone HLE + raw-PCM8 "OKI" channel, driven by a single command-byte latch | **wrong system for this game** — `wof`/`wofj` use QSound (§0, §6-of-real-doc... see the QSound register map below) | keep this file for non-QSound CPS-1 titles; add a **new, separate** `cps1_qsound_hle.c` for Tenchi wo Kurau II |

### QSound HLE target spec (CONFIRMED, `src/devices/sound/qsoundhle.{h,cpp}`)

MAME's own default (`qsound.h:71-77`: `QSOUND` aliases to `QSOUND_HLE` unless
`QSOUND_LLE` is defined) is *already* an HLE — a native C++ reimplementation of the
DSP16 chip's behavior, not a cycle-accurate DSP core emulation. This is the exact
"cheat A" philosophy this project already committed to, and the register map is the
concrete target to reimplement natively for the device:

**Z80-side wiring** (`cps1.cpp:663-671`, `qsound_sub_map`):
```
0xD000-0xD002  write: addr_hi, addr_lo, data-trigger (3 registers, see qsound_w below)
0xD007         read:  ready flag (0x00=busy, 0x80=ready)
0xC000-0xCFFF  shared RAM window 0 (68000 side: 0xF18000-0xF19FFF, 8KB, same array)
0xF000-0xFFFF  shared RAM window 1 (68000 side: 0xF1E000-0xF1FFFF, 8KB, same array)
```

**`qsound_w(offset, data)`** (`qsoundhle.cpp:193-213`):
```c
offset==0: data_latch = (data_latch & 0x00ff) | (data << 8);   // address high byte
offset==1: data_latch = (data_latch & 0xff00) | data;          // address low byte
offset==2: write_data(/*register address*/ data, data_latch);  // data itself = REGISTER ADDR
```
i.e. write the 16-bit VALUE across two bytes first (building `data_latch`), then write
the **8-bit register address** as the third byte to commit it — inverted from what
you'd naively guess (the "data" written last is the address, not the value).

**Register map** (`qsoundhle.cpp:233-266`, 256 possible addresses):
```
PCM voices 0-15, base = i*8:
  +0  bank        (applies to voice (i+1)%16 -- NOT to itself; a real hardware quirk)
  +1  addr        (current + start sample position)
  +2  rate        (4.12 fixed point)
  +3  phase
  +4  loop_len
  +5  end_addr
  +6  volume
  +7  unused
  0x80+i   voice pan
  0xBA+i   voice echo send level

ADPCM channels 0-2, base = 0xCA + i*4:
  +0  start_addr   +1  end_addr   +2  bank   +3  volume
  0xD6+i   flag (nonzero = start playback)
  0x90+i   channel pan

Global:
  0x93  echo feedback         0xD9  echo end_pos
  0xE2  delay_update trigger  0xE3  next_state
  per L/R (i=0,1):
    0xDA+2i  wet filter table_pos     0xDB+2i  dry (alt) filter table_pos
    0xDE+2i  wet delay                0xDF+2i  dry delay
    0xE4+2i  wet volume                0xE5+2i  dry volume
```

Sample rate: `60MHz / 2 (DSP core) / 1248 = ~24,038 Hz` (`qsound.h:20-21`).
16-bit samples read from ROM are 8-bit source shifted left 8
(`read_sample`, `qsoundhle.cpp:279-284`: `(int16_t)(sample_data << 8)`, "bit0-7 is
tied to ground" — i.e. QSound's PCM samples are 8-bit source data, not 16-bit).

**Honest scope call for the device port**: the 16-voice PCM path (bank/addr/rate/
phase/loop/volume/pan) is the audible core (music + most sfx) and is the right first
target — same phase-accumulator-resampling shape the Phase 6 skeleton already proved
out, just with real register semantics instead of invented ones. The FIR-filter/
echo/delay wet-dry chain (95-tap stereo filters + delay lines) is a real, measurable
DSP cost (2 filters × 95 taps × sample rate ≈ non-trivial MAC count per frame — exactly
the kind of thing `docs/CPS1_ULTIMATE_PORTING_PLAN.md`'s CMSIS-DSP suggestion was
aimed at) and should be a **separate, explicitly later milestone**, not bundled into
the first cut — a first pass that plays voices dry (skip the filter/echo mix) is
honestly still "sound HLE," just missing the reverb-like effect layer, and is a
reasonable place to stop before profiling whether the filter chain fits the budget.
ADPCM (3 channels) needs its own step-size decode table — QSound's ADPCM variant,
**not** OKI6295's (the Phase 6 skeleton's raw-PCM8 "OKI" stand-in isn't reusable here
either); MAME's `qsound_adpcm::update` (`qsoundhle.cpp`, not fully quoted above) is
the reference once that phase starts.

**Explicitly unconfirmed / needs the real ROM to finish**: the pan-table and
filter-coefficient-table DATA (`DATA_PAN_TAB=0x110`, `DATA_FILTER_TAB=0xd53`, etc.,
`qsoundhle.h:44-47`) live inside a **dumped QSound DSP sample ROM** MAME ships as part
of the driver's ROM set — this project has no CPS-1 ROMs (real or dumped) at all, so
those specific lookup tables are not obtainable from source-reading alone. Either
source them from an actual (legally-owned) ROM dump when one becomes available, or
derive an independent approximation later — flagged, not guessed at.

---

## 9. Concrete real-hardware integration plan

This is the phased plan for turning the skeleton into something that plays a real
Tenchi wo Kurau II ROM on the device, building on Phases 1-7's proven plumbing
(interpreter/recompiler diff, tile cache, bus dispatch, compositor, sound-command
intercept) rather than replacing it.

### Phase 8 — GFX/palette correctness pass (no new hardware, corrects §1/§2/§8)
1. Update `CPS1_TILE_SIZE_BYTES` to 64 and `CPS1_GFX_LAYOUT_DEFAULT` to §1's exact
   `planeoffset`/`xoffset`/`yoffset`; add the second (right-half) 8x8 layout and the
   confirmed 16x16/32x32 shapes as named constants (`CPS1_GFX_LAYOUT_8X8_LEFT/RIGHT`,
   `_16X16`, `_32X32`).
2. Add `cps1_palette_build(raw_word) -> rgb565` implementing §2's exact brightness
   formula; call it once per palette-base write (mirroring `cps1_cps_a_w`'s
   `if (offset == CPS1_PALETTE_BASE) cps1_build_palette(...)` trigger), not per pixel.
3. Regression gate: re-run `cps1-ppu-selftest`/`cps1-bg-selftest` with the corrected
   layout against the SAME synthetic byte patterns used today, hand-recompute the
   new expected packed-nibble output by the §1 formula, and update the test oracles
   — the whole point of those selftests is that they fail loudly the day this exact
   correction lands, which is now.

### Phase 9 — real memory map + indirect gfxram addressing (corrects §3/§4)
1. Replace `cps1_core.c`'s 5 fixed bus regions with: `mainram` at 0xFF0000 (unchanged),
   one 192KB `gfxram` byte array at 0x900000-0x92FFFF, and CPS-A registers at
   0x800100-0x80013F that store/mask base offsets per §4 (`cps1_base()`'s exact
   `*256` + boundary-mask + `&0x3ffff` logic).
2. OBJ/SCROLL1/2/3/palette/other all become **views into `gfxram` at their current
   base-register offset**, recomputed whenever a base register is written (mirroring
   `cps1_get_video_base()`), not separate fixed arrays.
3. Implement the one-frame OBJ delay (§5): a `cps1_objram_latch` step at the point in
   the frame loop that corresponds to the real vblank-adjacent latch, copying the
   *current* gfxram-relative OBJ view into a buffer the renderer reads instead.
4. New selftest: write through the CPU bus to a CPS-A base register, then to an
   address computed FROM that base (not a fixed constant), and confirm the sprite/
   tile ends up in the right place — proves the indirection, not just a fixed
   address, actually works.

### Phase 10 — sprite/BG field-layout + priority (corrects §5/§6)
1. Fix `cps1_oam_entry_t` to X,Y,tile,attr word order with the real attr bit layout;
   add multi-tile block-size sprite rendering (a sprite can be up to 16x16 blocks of
   16x16 tiles).
2. Fix `cps1_bg_cell_t` to code+attr with the real palette-offset-per-layer and
   flip bits; implement X/Y tile flip in `cps1_blit8x8_indexed` (not present at all
   today).
3. Implement the real tilemap scan/addressing (§6) so cell (col,row) maps to the
   confirmed bit-swizzled memory offset — required before loading any real VRAM
   layout, irrelevant for synthetic-only testing.
4. Priority: replace the strict bottom<middle<top compositor with the 2-bit
   priority-group + CPS-B priority-bitmask system (§4, §6). This is the single
   largest remaining design gap — budget it as its own sub-phase with MAME's
   `cps1_update_transmasks`/`cps1_render_layer`/`cps1_render_sprites` as the
   reference implementation to port the *behavior* of (transparency-mask multi-pass
   draws), not necessarily the *mechanism* (a tilemap-transmask redraw is exactly
   the kind of per-pixel software Z-buffer work cheat 8 exists to avoid — the CPU
   version should compute final priority once per pixel in the SCROLL3+sprite top
   buffer render, using the LTDC-layer split already in place, not by MAME's
   redraw-with-different-masks approach).

### Phase 11 — real ROM loading (corrects §7, extends `cps1_rom_linux.c`)
1. `wof`'s bank mapper (§7) is the simple case: all 4 gfx types share one unified
   64KB-tile-code space split into two 32KB banks. `cps1_rom_load_interleaved`
   (already built in Phase 4) handles the multi-chip byte-interleave; add a
   `gfxrom_bank_mapper`-equivalent (shift-by-type, linear-bank-for-wof) translation
   between a layer's raw tile code and the flat interleaved GFX blob offset.
2. Defer the general per-game `gfx_range` table system (needed for other CPS-1
   titles with disjoint per-type bank ranges) — not needed for this specific game.
3. PRG ROM: load and byte-interleave per the same convention; verify against the
   68000 reset vector (first two longwords = initial SSP/PC) actually pointing
   somewhere sane in the loaded PRG ROM as the first correctness gate, before
   trying to run further.

### Phase 12 — real hardware integration (LTDC, audio mixer)
1. **LTDC dual-layer binding** (cheat 8, now with a real target): SCROLL1 → LTDC
   Layer 1, SCROLL2 → LTDC Layer 2, hardware alpha/priority blend at scanout.
   SCROLL3 + sprites render into the single CPU buffer already proven in Phase 7's
   `cps1_core_run_frame_device_cost()` — that split is already correct in shape;
   what's missing is feeding LTDC real pixel-format buffers (device work, not yet
   started) instead of a host compositor stand-in.
2. **QSound HLE → device audio pipeline**: the 16-voice PCM mixer (§8) writes into
   the same kind of ring buffer the existing `cps1_sound_hle_mix()` already
   populates (`Core/Src/porting/odroid_audio.c`'s SAI DMA path is the existing,
   proven device audio sink for every other core in this repo — reuse it, don't
   reinvent it).
3. **Profiling gate**: re-run the Phase 7 QEMU M7 rig methodology
   (`tools/m7_qemu_rig/run_cps1.sh`) once Phases 8-11 land, comparing against the
   SAME 16.6ms/5.6M-cycle budget — the real GFX/priority/sound work will cost more
   instructions than the Phase 7 synthetic scene did, and the 69.7% cheat-8 saving
   measured then needs re-confirming with real (or at least correctly-shaped)
   content, not re-assumed.

---

## 10. Sources (fetched into this session, re-fetch to verify)

```
https://raw.githubusercontent.com/mamedev/mame/master/src/mame/capcom/cps1.cpp
https://raw.githubusercontent.com/mamedev/mame/master/src/mame/capcom/cps1.h
https://raw.githubusercontent.com/mamedev/mame/master/src/mame/capcom/cps1_v.cpp
https://raw.githubusercontent.com/mamedev/mame/master/src/devices/sound/qsound.h
https://raw.githubusercontent.com/mamedev/mame/master/src/devices/sound/qsound.cpp
https://raw.githubusercontent.com/mamedev/mame/master/src/devices/sound/qsoundhle.h
https://raw.githubusercontent.com/mamedev/mame/master/src/devices/sound/qsoundhle.cpp
```

All fetched via `curl` directly (not MAME's own repo, not vendored into this tree —
MAME is GPL/BSD-mixed and not a dependency of this project; this document is notes
derived from reading it, not a copy of its source).
