#pragma once
/*
 * CPS-1 background scroll-layer renderer + compositor -- skeleton for
 * [cheat 8] in docs/CPS1_SENIOR_TRICKS_ANALYSIS.md: on real hardware,
 * SCROLL1 (bottom) and SCROLL2 (middle) bind to the STM32H7 LTDC's two
 * hardware overlay layers, so blending them costs the CPU nothing -- only
 * SCROLL3 (top bg layer) + sprites are ever CPU-rendered, into one buffer
 * (see cps1_ppu.c). cps1_compositor_blend_priority() below is a HOST-ONLY
 * stand-in for what LTDC does in hardware, so this skeleton is checkable
 * without real silicon; blending bottom+middle is NOT part of the device's
 * per-frame CPU cost once real LTDC layers replace it -- and, since LTDC
 * only ever sees final RGB565 pixels (no pen-index/priority-group
 * metadata), SCROLL1/SCROLL2 tiles CANNOT punch over sprites on real
 * hardware under this cheat, even though real CPS-1 silicon could: this is
 * a genuine, accepted limitation of cheat 8's architecture, not a bug (see
 * cps1_core_run_frame_device_cost's comment in cps1_core.c).
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

/*
 * Cell format: 2 words, code then attr -- CONFIRMED
 * (docs/CPS1_MAME_ALIGNMENT.md section 6, cps1_v.cpp:2434-2507). `code` is
 * the base 8x8 sub-tile index (see file header for the sub-tile
 * decomposition convention); `attr`'s bits are decoded on demand by the
 * accessors below rather than pre-split into separate struct fields, to
 * mirror the real "code+attr" wire format literally instead of adding
 * skeleton-only derived fields (e.g. the Phase 5-9 skeleton's invented
 * "enabled" bit, which is GONE -- real hardware has no per-cell on/off
 * flag, only per-PIXEL transparency via pen index 0, and that invented bit
 * collided with attr's own real bit 8 (part of the priority-group field)
 * once attr became the literal real word).
 */
typedef struct {
    uint16_t code;
    uint16_t attr;
} cps1_bg_cell_t;

#define CPS1_BG_ATTR_COLOR_MASK 0x1Fu
#define CPS1_BG_ATTR_FLIP_X     0x0020u
#define CPS1_BG_ATTR_FLIP_Y     0x0040u
#define CPS1_BG_ATTR_PRIORITY_SHIFT 7
#define CPS1_BG_ATTR_PRIORITY_MASK  0x3u

static inline unsigned cps1_bg_attr_color(uint16_t attr) { return attr & CPS1_BG_ATTR_COLOR_MASK; }
static inline unsigned cps1_bg_attr_flip_x(uint16_t attr) { return (attr & CPS1_BG_ATTR_FLIP_X) != 0; }
static inline unsigned cps1_bg_attr_flip_y(uint16_t attr) { return (attr & CPS1_BG_ATTR_FLIP_Y) != 0; }
static inline unsigned cps1_bg_attr_priority(uint16_t attr)
{
    return (attr >> CPS1_BG_ATTR_PRIORITY_SHIFT) & CPS1_BG_ATTR_PRIORITY_MASK;
}

/* layer_palette_offset: SCROLL1=+0x20, SCROLL2=+0x40, SCROLL3=+0x60
 * (CONFIRMED, cps1_v.cpp:2464/2484/2502) -- added to a cell's raw 0-31
 * color field to reach its real palette bank (0-127, CPS1_PALETTE_BANKS). */
static inline unsigned cps1_bg_layer_palette_offset(unsigned layer_index)
{
    switch (layer_index) {
    case CPS1_BG_SCROLL1: return 0x20u;
    case CPS1_BG_SCROLL2: return 0x40u;
    default:              return 0x60u; /* CPS1_BG_SCROLL3 */
    }
}

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

/*
 * Tilemap addressing is bit-swizzled, not row-major -- CONFIRMED
 * (docs/CPS1_MAME_ALIGNMENT.md section 6, cps1_v.cpp:2434-2448). A real
 * 68000 program addresses tilemap RAM by this swizzled cell OFFSET, not by
 * col+row*64; cps1_bg_swizzle_col_row_to_offset is the forward direction
 * (used by test programs / real ROM code that WANTS to write cell
 * (col,row)); cps1_bg_swizzle_offset_to_col_row is its exact inverse (used
 * by cps1_core.c's bus dispatch to find which cells[] slot a raw address's
 * swizzled offset actually targets, so cells[] itself can stay simple
 * logical row-major storage for the renderer). Both are pure bit
 * rearrangement (no aliasing/collisions across the full 64x64 domain), not
 * an approximation -- exact for col,row < 64.
 */
uint32_t cps1_bg_swizzle_col_row_to_offset(unsigned layer_index, unsigned col, unsigned row);
void cps1_bg_swizzle_offset_to_col_row(unsigned layer_index, uint32_t offset,
                                        unsigned *out_col, unsigned *out_row);

/* Renders one layer into out_fb (CPS1_FB_WIDTH x CPS1_FB_HEIGHT, RGB565),
 * stamping out_meta (same dimensions, see CPS1_PIXEL_META_* in cps1_ppu.h)
 * at every pixel it draws so the priority compositor can test punch-
 * through without re-decoding the tile. Does NOT clear out_fb/out_meta
 * first -- the caller decides whether this layer starts from a cleared
 * buffer or accumulates. Index-0 pixels are left untouched (transparent);
 * there is no per-cell on/off gate any more (see cps1_bg_cell_t's comment
 * above) -- every cell is always attempted, matching real hardware. */
void cps1_bg_render_layer_ex(const cps1_bg_layer_t *layer, unsigned layer_index,
                              const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                              const cps1_palette_t *pal, uint16_t *out_fb, uint8_t *out_meta,
                              const cps1_cover_t *skip, cps1_cover_t *emit);

void cps1_bg_render_layer(const cps1_bg_layer_t *layer, unsigned layer_index,
                           const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                           const cps1_palette_t *pal, uint16_t *out_fb, uint8_t *out_meta);

/* CPS-B priority-bitmask registers (4 of them -- CONFIRMED,
 * docs/CPS1_MAME_ALIGNMENT.md sections 4/6, cps1_v.cpp:328-334): bit `pen`
 * set in masks[priority_group] means a BG pixel of that priority group
 * whose raw pen value is `pen` draws OVER sprites, even though sprites
 * otherwise draw on top of all three BG layers. This is the plain-English
 * hardware behavior from the confirmed source comment ("indicate pens in
 * the tile that have priority over sprites") -- deliberately NOT a port of
 * MAME's internal tilemap set_transmask()/^0xffff bookkeeping, which is an
 * implementation-mechanism detail docs/CPS1_MAME_ALIGNMENT.md section 9's
 * Phase 10 plan explicitly says to not replicate (port the BEHAVIOR, not
 * the mechanism). */
typedef struct {
    uint16_t masks[4];
} cps1_priority_masks_t;

/* Composites `layer_count` BG layers (bottom to top -- index 0 is the
 * bottommost, e.g. SCROLL1) plus one sprite layer using per-pixel
 * priority-group + CPS-B mask punch-through, REPLACING the old
 * unconditional bottom<middle<top order. `colors`/`metas` are
 * layer_count-long arrays of CPS1_FB_WIDTH*CPS1_FB_HEIGHT buffers (parallel
 * arrays -- colors[i]/metas[i] is one layer). Normal BG-vs-BG order is
 * unaffected by priority masks (a higher-index opaque layer always wins
 * over a lower one, same as before); only the BG-vs-SPRITE decision at each
 * pixel consults masks. layer_count may be 1 (cps1_core_run_frame_device_
 * cost's SCROLL3-only device-realistic path) or 3 (cps1_core_run_frame's
 * full host-verification path, SCROLL1/2/3) -- see cps1_core.c. */
void cps1_compositor_blend_priority(const uint16_t *const *colors,
                                     const uint8_t *const *metas,
                                     unsigned layer_count,
                                     const uint16_t *sprite_fb,
                                     const cps1_priority_masks_t *masks,
                                     uint16_t *out);
