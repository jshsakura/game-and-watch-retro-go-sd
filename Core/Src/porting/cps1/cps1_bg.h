#pragma once
/*
 * CPS-1 background scroll-layer renderer + compositor -- skeleton for
 * [cheat 8] in docs/CPS1_SENIOR_TRICKS_ANALYSIS.md: on real hardware,
 * SCROLL1 (bottom) and SCROLL2 (middle) bind to the STM32H7 LTDC's two
 * hardware overlay layers, so blending them costs the CPU nothing -- only
 * SCROLL3 (top bg layer) + sprites are ever CPU-rendered, into one buffer
 * (see cps1_ppu.c). cps1_compositor_blend() below is a HOST-ONLY stand-in
 * for what LTDC does in hardware, so this skeleton is checkable without
 * real silicon; blending bottom+middle is NOT part of the device's
 * per-frame CPU cost once real LTDC layers replace it.
 *
 * Reuses the EXISTING 8x8 tile cache/decode (cps1_ppu.h) for all three
 * layers -- SCROLL2's 16x16 and SCROLL3's 32x32 tiles are just 2x2 / 4x4
 * grids of 8x8 sub-tiles at consecutive tile indices (base + row*side +
 * col), a common arcade-era convention, so no second cache is needed.
 *
 * CONFIRMED: SCROLL1/2/3 use 8x8/16x16/32x32 tiles respectively (multiple
 * independent sources). NOT confirmed: tilemap grid dimensions --
 * CPS1_BG_MAP_W/H (64x64) are a placeholder, not a verified CPS-1 value.
 *
 * Transparency convention matches cps1_ppu.c: index 0 is transparent and
 * left unwritten, so a layer buffer that starts zeroed and is never told
 * to fill index 0 doubles as "nothing here yet" for the compositor -- the
 * same accepted simplification cps1_ppu_render already uses (can't tell
 * "explicitly black" from "untouched"; not distinguished on real hardware
 * either, which uses a separate alpha channel this skeleton doesn't have).
 */
#include <stdint.h>

#include "cps1_core.h"
#include "cps1_ppu.h"
#include "cps1_rom.h"

#define CPS1_BG_LAYER_COUNT 3
#define CPS1_BG_MAP_W       64
#define CPS1_BG_MAP_H       64

enum {
    CPS1_BG_SCROLL1 = 0, /* bottom -- 8x8   tiles -- LTDC hardware Layer 1 on device */
    CPS1_BG_SCROLL2 = 1, /* middle -- 16x16 tiles -- LTDC hardware Layer 2 on device */
    CPS1_BG_SCROLL3 = 2, /* top bg -- 32x32 tiles -- CPU-rendered, shares the sprite buffer */
};

typedef struct {
    uint16_t tile_index; /* base 8x8 sub-tile index (see file header) */
    uint8_t palette;      /* palette bank */
    uint8_t enabled;
} cps1_bg_cell_t;

typedef struct {
    cps1_bg_cell_t cells[CPS1_BG_MAP_W * CPS1_BG_MAP_H];
    int16_t scroll_x, scroll_y;
} cps1_bg_layer_t;

typedef struct {
    cps1_bg_layer_t layers[CPS1_BG_LAYER_COUNT];
} cps1_bg_state_t;

void cps1_bg_reset(cps1_bg_state_t *bg);

/* Tile size in pixels for a given CPS1_BG_SCROLLn layer index (8/16/32). */
unsigned cps1_bg_tile_px(unsigned layer_index);

/* Renders one layer into out_fb (CPS1_FB_WIDTH x CPS1_FB_HEIGHT, RGB565).
 * Does NOT clear out_fb first -- the caller decides whether this layer
 * should start from a cleared buffer (bottom, usually) or accumulate onto
 * an existing one (top, shared with sprites). Disabled cells and index-0
 * pixels are left untouched. */
void cps1_bg_render_layer(const cps1_bg_layer_t *layer, unsigned layer_index,
                           const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                           const cps1_palette_t *pal, uint16_t *out_fb);

/* Host-only LTDC stand-in: bottom always shows (no layer under it);
 * middle/top show wherever they're non-zero (see transparency note above),
 * otherwise whatever's under them shows through. On the device this
 * bottom+middle blend is free (LTDC hardware); only rendering `top`
 * (already composited by the caller: SCROLL3 render + cps1_ppu_render) is
 * a real per-frame CPU cost. */
void cps1_compositor_blend(const uint16_t *bottom, const uint16_t *middle,
                            const uint16_t *top, uint16_t *out);
