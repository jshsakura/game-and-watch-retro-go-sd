#pragma once
/*
 * CPS-1 PPU skeleton: OAM prescan (spatial pre-filtering) + a 256KB
 * direct-mapped LRU tile cache, per docs/CPS1_ULTIMATE_PORTING_PLAN.md
 * techniques 3 and 5. Freestanding (stdint.h only) so it compiles
 * unmodified on host and device, matching every other cps1_* core file.
 *
 * Caches tiles PACKED 4bpp (32 bytes/tile, technique 3), not decoded to
 * RGB565 -- that halves storage vs. a 16bpp cache, at the cost of a
 * palette lookup at blit time (not implemented here; blitting is a later
 * milestone once a real PPU renderer exists).
 */
#include <stdint.h>

#include "cps1_core.h"
#include "cps1_rom.h"

#define CPS1_OAM_MAX_SPRITES 256

/*
 * Attribute word bit layout -- CONFIRMED (docs/CPS1_MAME_ALIGNMENT.md
 * section 5, cps1_v.cpp:2649-2668): color occupies 5 bits (0-31, not 0-15
 * as the Phase 5 skeleton's uint8_t attr field implied), and bits 8-15
 * hold multi-tile block-size nibbles that a uint8_t can't even represent
 * -- attr must be uint16_t.
 */
#define CPS1_OAM_ATTR_COLOR_MASK    0x1Fu
#define CPS1_OAM_ATTR_FLIP_X        0x0020u
#define CPS1_OAM_ATTR_FLIP_Y        0x0040u
#define CPS1_OAM_ATTR_XY_TOGGLE     0x0080u /* Marvel vs. Capcom only -- unused */
#define CPS1_OAM_ATTR_XBLOCK_SHIFT  8
#define CPS1_OAM_ATTR_YBLOCK_SHIFT  12
#define CPS1_OAM_ATTR_BLOCK_MASK    0x0Fu

typedef struct {
    int16_t x, y;
    uint16_t tile_index; /* base 16x16-unit index -- see cps1_ppu.c file
                             header for the 8x8-sub-tile decomposition
                             convention this skeleton uses instead of the
                             real CPS1_GFX_LAYOUT_16X16 raw decode. */
    uint16_t attr;
    uint8_t enabled;
} cps1_oam_entry_t;

typedef struct {
    cps1_oam_entry_t sprites[CPS1_OAM_MAX_SPRITES];
    uint32_t count;
} cps1_oam_t;

/* OAM prescan: culls sprites that cannot overlap the CPS1_FB_WIDTH x
 * CPS1_FB_HEIGHT viewport at all (an 8x8 tile fully off to one side), so
 * only visible tile IDs reach the cache. Writes up to max_out tile IDs
 * into out_visible_tile_ids and returns how many it wrote. */
uint32_t cps1_oam_prescan(const cps1_oam_t *oam, uint16_t *out_visible_tile_ids,
                           uint32_t max_out);

typedef struct {
    uint32_t last_used;
    uint32_t tile_index;   /* flat index, or a composed block key (see below) */
    uint8_t valid;
    uint8_t pixels[CPS1_TILE_SIZE_BYTES];
} cps1_tile_cache_slot_t;

/* Slot count derived from sizeof(slot), not a naive 256KB/32B division --
 * per-slot metadata (last_used/tile_index/valid) is real RAM cost too, so
 * the array is sized to make its OWN total footprint land at 256KB,
 * instead of quietly costing more than the budgeted number. */
/* Overridable so the device build can trade cache for RAM_EMU headroom. The
 * 256 KB default is what the host harnesses and every selftest size against;
 * the device build passes a smaller figure (see main_cps1.c's memory plan) --
 * this is a CACHE, so shrinking it costs hit rate, not correctness. */
#ifndef CPS1_TILE_CACHE_BUDGET_BYTES
#define CPS1_TILE_CACHE_BUDGET_BYTES (256 * 1024)
#endif
enum { CPS1_TILE_CACHE_SLOTS = (int)CPS1_TILE_CACHE_BUDGET_BYTES / (int)sizeof(cps1_tile_cache_slot_t) };

typedef struct {
    cps1_tile_cache_slot_t slots[CPS1_TILE_CACHE_SLOTS];
    uint32_t clock;
    uint32_t hits;
    uint32_t misses;
    /*
     * 0 -> misses are served by cps1_rom_decode_tile(), the flat
     *      "index * 32 bytes" reader. Correct only when rom->gfx is already
     *      packed 4bpp, which is true of cps1_core.c's synthetic data and of
     *      every selftest built on it, so this stays the default.
     * 1 -> misses are served by cps1_rom_decode_subtile() with the layer's
     *      real geometry. A real CPS-1 GFX dump gives each of SCROLL1/2/3 a
     *      DIFFERENT byte layout (8/8/16-byte rows, 1/2/4 spans per row), so
     *      decoding all three as 8x8 renders SCROLL1 correctly and scrambles
     *      the other two -- which is exactly how the first real title screen
     *      came out.
     */
    uint8_t real_gfx;
} cps1_tile_cache_t;

void cps1_tile_cache_reset(cps1_tile_cache_t *cache);

/* Returns a pointer to the packed 4bpp tile data for tile_index (32 bytes,
 * owned by the cache -- valid until evicted). On a miss, decodes it from
 * `rom` (the "Flash dump" path) into the direct-mapped slot
 * tile_index % CPS1_TILE_CACHE_SLOTS, evicting whatever was there.
 * Returns NULL if the ROM can't supply that tile (see
 * cps1_rom_decode_tile). */
const uint8_t *cps1_tile_cache_fetch(cps1_tile_cache_t *cache, const cps1_rom_t *rom,
                                      uint32_t tile_index);

/* Palette RAM: 128 banks x 16 colors (docs/CPS1_MAME_ALIGNMENT.md section 6:
 * sprites use banks 0x00-0x1F directly; SCROLL1/2/3 cells add a per-layer
 * offset of +0x20/+0x40/+0x60 to their own 0-31 color field, reaching up to
 * bank 0x7F -- GFXDECODE's shared "0x80 total color groups" is what fixes
 * this at 128, not 32). Index 0/bank is transparent by convention (never
 * written to the framebuffer). Storage here is already-converted RGB565,
 * built from the real raw hardware word via cps1_palette_build() below --
 * see that function's doc comment for the raw format. */
#define CPS1_PALETTE_BANKS  128
#define CPS1_PALETTE_COLORS 16

typedef struct {
    uint16_t colors[CPS1_PALETTE_BANKS][CPS1_PALETTE_COLORS];
} cps1_palette_t;

/*
 * Converts one raw CPS-1 palette word to RGB565 -- CONFIRMED against MAME
 * source (docs/CPS1_MAME_ALIGNMENT.md section 2, cps1_v.cpp's
 * cps1_build_palette): raw word is 12-bit RGB + 4-bit brightness, NOT
 * direct RGB565.
 *
 *   bits 15-12  brightness (0x0 = scales to 1/3, not black -- a deliberate
 *               hardware quirk used for fades, not a bug)
 *   bits 11-8   R (4 bits)
 *   bits 7-4    G (4 bits)
 *   bits 3-0    B (4 bits)
 *
 *   bright = 0x0f + (brightness_nibble << 1)          // 0x0f..0x2d
 *   r8 = R_nibble * 0x11 * bright / 0x2d               // nibble -> byte, scaled
 *   g8 = G_nibble * 0x11 * bright / 0x2d
 *   b8 = B_nibble * 0x11 * bright / 0x2d
 *   rgb565 = (r8>>3)<<11 | (g8>>2)<<5 | (b8>>3)
 *
 * On real hardware this conversion runs once per palette-base-register
 * write, over a whole page of raw words already staged in gfxram (MAME's
 * cps1_build_palette) -- NOT once per individual RAM byte-write. The
 * Phase 1-7 skeleton's bus doesn't yet have that staged/paged gfxram
 * indirection (docs/CPS1_MAME_ALIGNMENT.md section 9, Phase 9), so for now
 * this is called inline at each palette-word bus write in cps1_core.c --
 * the end result (an RGB565 value derived from this formula) is the same;
 * only the batching/trigger-timing is simplified until Phase 9 lands.
 */
uint16_t cps1_palette_build(uint16_t raw);

/* Renders every visible OAM sprite into `fb` (CPS1_FB_WIDTH x
 * CPS1_FB_HEIGHT, RGB565, NOT cleared by this function -- caller clears
 * first). Sprite base unit is 16x16 (CONFIRMED, docs/CPS1_MAME_ALIGNMENT.md
 * section 5 -- gfx(2)'s real layout, NOT 8x8), decomposed as a 2x2 grid of
 * 8x8 sub-tiles (cps1_blit_block_indexed below) at consecutive tile
 * indices, same convention cps1_bg.c already uses for SCROLL2/3 -- real
 * CPS1_GFX_LAYOUT_16X16 raw decode is deferred to Phase 11 (real ROM
 * loading). Multi-tile block sprites (attr bits 8-15) iterate
 * (xblock+1)*(yblock+1) such units; X/Y flip mirrors both the block
 * arrangement and each unit's own pixels, matching MAME's
 * cps1_render_sprites DRAWSPRITE loop. Sprites never carry priority-group
 * metadata themselves (only BG cells do) -- they are always the single
 * "top" input to cps1_compositor_blend_priority. */
void cps1_ppu_render(const cps1_oam_t *oam, const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                      const cps1_palette_t *pal, uint16_t *fb);

/* Packed per-pixel metadata cps1_blit8x8_indexed/cps1_blit_block_indexed
 * stamp alongside a BG pixel's color, so cps1_compositor_blend_priority can
 * later test "does this exact pixel's (priority_group, pen) have priority
 * over sprites" without re-decoding the tile. Bit 7 = valid (a pixel was
 * actually drawn here, vs. untouched backdrop); bits 4-5 = priority_group
 * (0-3); bits 0-3 = raw pen/color-index (0-15) BEFORE the palette lookup.
 * Sprites don't produce this (they pass out_meta = NULL). */
#define CPS1_PIXEL_META_VALID       0x80u
#define CPS1_PIXEL_META_GROUP_SHIFT 4
#define CPS1_PIXEL_META_GROUP_MASK  0x3u
#define CPS1_PIXEL_META_PEN_MASK    0xFu

/* Shared by cps1_ppu_render (sprites) and cps1_bg.c (scroll layers): unpacks
 * one cached 4bpp tile's nibbles (mirrored per flip_x/flip_y) and writes
 * non-transparent (index != 0) pixels into fb at (dst_x,dst_y), clipped to
 * CPS1_FB_WIDTH/HEIGHT. If out_meta is non-NULL (BG callers only -- see
 * CPS1_PIXEL_META_* above), stamps the same clipped positions in out_meta
 * (same dimensions as fb) with priority_group + the raw pen index, so the
 * priority compositor can test punch-through without re-fetching the tile. */
void cps1_blit8x8_indexed(const uint8_t *tile4bpp, unsigned palette_bank,
                           const cps1_palette_t *pal, int dst_x, int dst_y,
                           int flip_x, int flip_y, uint16_t *fb,
                           uint8_t *out_meta, uint8_t priority_group);

/* Draws a sub x sub grid of 8x8 sub-tiles (sub=1/2/4 -> 8x8/16x16/32x32) as
 * one flip_x/flip_y-mirrored square block at (dst_x,dst_y): fetches
 * base_subtile + row*sub + col (row/col already flip-adjusted so the WHOLE
 * block mirrors, not just each sub-tile's own pixels) through `cache`,
 * blitting each via cps1_blit8x8_indexed above. Shared by cps1_ppu_render's
 * 16x16 sprite units and cps1_bg_render_layer's SCROLL2/3 sub-tile
 * decomposition (docs/CPS1_MAME_ALIGNMENT.md sections 5/6) -- one place
 * implements "flip a composed multi-sub-tile block" instead of two. */
/* Fetches the (qx,qy) 8x8 quadrant of a sub x sub block whose tile code is
 * `code`, decoding through the layer's real geometry. Cache key composes all
 * four so blocks of different sizes cannot collide. */
const uint8_t *cps1_tile_cache_fetch_block(cps1_tile_cache_t *cache, const cps1_rom_t *rom,
                                            unsigned sub, uint32_t code,
                                            unsigned qx, unsigned qy);

void cps1_blit_block_indexed(uint32_t base_subtile, unsigned sub, unsigned palette_bank,
                              const cps1_palette_t *pal, int dst_x, int dst_y,
                              int flip_x, int flip_y, cps1_tile_cache_t *cache,
                              const cps1_rom_t *rom, uint16_t *fb,
                              uint8_t *out_meta, uint8_t priority_group);
