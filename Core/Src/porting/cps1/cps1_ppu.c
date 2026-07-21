#include <stddef.h>

#include "cps1_ppu.h"

uint32_t cps1_oam_prescan(const cps1_oam_t *oam, uint16_t *out_visible_tile_ids,
                           uint32_t max_out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < oam->count && n < max_out; i++) {
        const cps1_oam_entry_t *s = &oam->sprites[i];
        if (!s->enabled)
            continue;
        /* Spatial pre-filter (technique 5): a sprite's REAL footprint is
         * (xblock+1)*(yblock+1) 16x16 units (docs/CPS1_MAME_ALIGNMENT.md
         * section 5), not a fixed 8x8 -- using the old fixed size here
         * would cull sprites the real renderer still draws. */
        unsigned nx = ((s->attr >> CPS1_OAM_ATTR_XBLOCK_SHIFT) & CPS1_OAM_ATTR_BLOCK_MASK) + 1u;
        unsigned ny = ((s->attr >> CPS1_OAM_ATTR_YBLOCK_SHIFT) & CPS1_OAM_ATTR_BLOCK_MASK) + 1u;
        if (s->x + (int)(nx * 16u) <= 0 || s->x >= CPS1_FB_WIDTH)
            continue;
        if (s->y + (int)(ny * 16u) <= 0 || s->y >= CPS1_FB_HEIGHT)
            continue;
        out_visible_tile_ids[n++] = s->tile_index;
    }
    return n;
}

void cps1_tile_cache_reset(cps1_tile_cache_t *cache)
{
    cache->clock = 0;
    cache->hits = 0;
    cache->misses = 0;
    /* MUST be cleared here: a cache that is not statically zero-initialised
     * (any stack or ram_malloc'd one -- cps1-ppu-selftest has such a one)
     * otherwise carries a garbage pointer into the miss path and segfaults
     * on the first fetch. Callers wanting real-ROM decoding set ->layout
     * AFTER calling this. */
    cache->layout = NULL;
    for (int i = 0; i < CPS1_TILE_CACHE_SLOTS; i++) {
        cache->slots[i].valid = 0;
        cache->slots[i].last_used = 0;
        cache->slots[i].tile_index = 0;
    }
}

static uint32_t slot_for_tile(uint32_t tile_index)
{
    /* Direct-mapped: collisions are resolved by eviction on the next
     * fetch, not by a second associative way. Simplest thing that can
     * hold a real hit-rate measurement -- revisit if that measurement
     * shows too many evictions on real ROM access patterns. */
    return tile_index % (uint32_t)CPS1_TILE_CACHE_SLOTS;
}

const uint8_t *cps1_tile_cache_fetch(cps1_tile_cache_t *cache, const cps1_rom_t *rom,
                                      uint32_t tile_index)
{
    uint32_t idx = slot_for_tile(tile_index);
    cps1_tile_cache_slot_t *slot = &cache->slots[idx];
    cache->clock++;

    if (slot->valid && slot->tile_index == tile_index) {
        cache->hits++;
        slot->last_used = cache->clock;
        return slot->pixels;
    }

    cache->misses++;
    int rc = cache->layout ? cps1_rom_decode_tile_planar(rom, cache->layout, tile_index, slot->pixels)
                           : cps1_rom_decode_tile(rom, tile_index, slot->pixels);
    if (rc != 0) {
        slot->valid = 0;
        return NULL;
    }
    slot->tile_index = (uint16_t)tile_index;
    slot->valid = 1;
    slot->last_used = cache->clock;
    return slot->pixels;
}

uint16_t cps1_palette_build(uint16_t raw)
{
    unsigned brightness = (raw >> 12) & 0xFu;
    unsigned r_nibble = (raw >> 8) & 0xFu;
    unsigned g_nibble = (raw >> 4) & 0xFu;
    unsigned b_nibble = raw & 0xFu;

    unsigned bright = 0x0Fu + (brightness << 1); /* 0x0f..0x2d */

    unsigned r8 = r_nibble * 0x11u * bright / 0x2Du;
    unsigned g8 = g_nibble * 0x11u * bright / 0x2Du;
    unsigned b8 = b_nibble * 0x11u * bright / 0x2Du;

    unsigned r5 = (r8 >> 3) & 0x1Fu;
    unsigned g6 = (g8 >> 2) & 0x3Fu;
    unsigned b5 = (b8 >> 3) & 0x1Fu;

    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* One pixel of the unrolled fast path (see cps1_blit8x8_indexed below) --
 * identical semantics to the slow path's per-pixel body (transparent pens
 * skipped, meta stamped only when idx != 0), just called with a compile-
 * time-known destination slot instead of a loop-computed one. */
static inline void cps1_blit_pixel(uint16_t *dst, uint8_t *meta, uint8_t idx,
                                    unsigned bank, const cps1_palette_t *pal,
                                    uint8_t meta_prefix)
{
    if (idx) {
        *dst = pal->colors[bank][idx];
        if (meta)
            *meta = (uint8_t)(meta_prefix | (idx & CPS1_PIXEL_META_PEN_MASK));
    }
}

/* Fully unrolled 8-pixel row, no bounds checks -- caller (the fast path in
 * cps1_blit8x8_indexed) has already proven the whole row is on-screen.
 * tile_row is the 4 packed bytes for this row (8 nibbles); the col->
 * (byte,nibble) mapping and flip_x direction are traced by hand against
 * the ORIGINAL per-pixel formula (src_col = flip_x ? 7-col : col; idx =
 * (src_col&1) ? byte&0xF : byte>>4, byte = tile_row[src_col/2]) in the
 * Phase-13 optimization commit -- not re-derived from this code, so a
 * regression here shows up as a real pixel mismatch, not a tautology. */
CPS1_ITCM_TEXT
static void cps1_blit8x8_row_fast(const uint8_t *tile_row, unsigned bank,
                                   const cps1_palette_t *pal, int flip_x,
                                   uint16_t *dst_row, uint8_t *meta_row,
                                   uint8_t meta_prefix)
{
    uint8_t b0 = tile_row[0], b1 = tile_row[1], b2 = tile_row[2], b3 = tile_row[3];

    /* Each pixel writes its OWN meta slot (&meta_row[N], not a shared
     * pointer) -- the compositor indexes meta per-column just like the
     * color buffer, so a shared pointer here would silently discard every
     * column's meta but column 0's, breaking priority compositing while
     * leaving raw per-layer colors looking correct (caught by cps1-bg-
     * selftest's compositor-level checks, not its per-layer sanity ones --
     * see the Phase-13 commit for that exact regression trace). */
    if (!flip_x) {
        cps1_blit_pixel(&dst_row[0], meta_row ? &meta_row[0] : NULL, (uint8_t)(b0 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[1], meta_row ? &meta_row[1] : NULL, (uint8_t)(b0 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[2], meta_row ? &meta_row[2] : NULL, (uint8_t)(b1 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[3], meta_row ? &meta_row[3] : NULL, (uint8_t)(b1 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[4], meta_row ? &meta_row[4] : NULL, (uint8_t)(b2 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[5], meta_row ? &meta_row[5] : NULL, (uint8_t)(b2 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[6], meta_row ? &meta_row[6] : NULL, (uint8_t)(b3 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[7], meta_row ? &meta_row[7] : NULL, (uint8_t)(b3 & 0x0Fu), bank, pal, meta_prefix);
    } else {
        cps1_blit_pixel(&dst_row[0], meta_row ? &meta_row[0] : NULL, (uint8_t)(b3 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[1], meta_row ? &meta_row[1] : NULL, (uint8_t)(b3 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[2], meta_row ? &meta_row[2] : NULL, (uint8_t)(b2 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[3], meta_row ? &meta_row[3] : NULL, (uint8_t)(b2 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[4], meta_row ? &meta_row[4] : NULL, (uint8_t)(b1 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[5], meta_row ? &meta_row[5] : NULL, (uint8_t)(b1 >> 4), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[6], meta_row ? &meta_row[6] : NULL, (uint8_t)(b0 & 0x0Fu), bank, pal, meta_prefix);
        cps1_blit_pixel(&dst_row[7], meta_row ? &meta_row[7] : NULL, (uint8_t)(b0 >> 4), bank, pal, meta_prefix);
    }
}

CPS1_ITCM_TEXT
void cps1_blit8x8_indexed(const uint8_t *tile4bpp, unsigned palette_bank,
                           const cps1_palette_t *pal, int dst_x, int dst_y,
                           int flip_x, int flip_y, uint16_t *fb,
                           uint8_t *out_meta, uint8_t priority_group)
{
    unsigned bank = palette_bank & (CPS1_PALETTE_BANKS - 1);
    uint8_t meta_prefix = (uint8_t)(CPS1_PIXEL_META_VALID |
                                     ((priority_group & CPS1_PIXEL_META_GROUP_MASK)
                                      << CPS1_PIXEL_META_GROUP_SHIFT));

    /* Fast path: the whole 8x8 tile is on-screen (the overwhelmingly
     * common case -- only tiles straddling a screen edge ever miss this),
     * so every row/column bounds check the slow path needs is provably
     * unnecessary here. Per-row, the inner column loop is fully unrolled
     * (cps1_blit8x8_row_fast) -- 8 fixed pixel slots instead of a
     * counted loop, matching the Phase-13 optimization-phase ask. */
    if (dst_x >= 0 && dst_x + 8 <= CPS1_FB_WIDTH && dst_y >= 0 && dst_y + 8 <= CPS1_FB_HEIGHT) {
        for (int row = 0; row < 8; row++) {
            int src_row = flip_y ? (7 - row) : row;
            int dst_off = (dst_y + row) * CPS1_FB_WIDTH + dst_x;
            cps1_blit8x8_row_fast(tile4bpp + src_row * 4, bank, pal, flip_x,
                                   fb + dst_off, out_meta ? out_meta + dst_off : NULL,
                                   meta_prefix);
        }
        return;
    }

    /* Slow path: tile straddles a screen edge -- keep the original,
     * simple per-pixel-checked loop (correctness over speed; this is the
     * rare case a real game's scrolling only hits at the visible border). */
    for (int row = 0; row < 8; row++) {
        int py = dst_y + row;
        if (py < 0 || py >= CPS1_FB_HEIGHT)
            continue;
        int src_row = flip_y ? (7 - row) : row;
        for (int col = 0; col < 8; col++) {
            int px = dst_x + col;
            if (px < 0 || px >= CPS1_FB_WIDTH)
                continue;
            int src_col = flip_x ? (7 - col) : col;
            uint8_t byte = tile4bpp[src_row * 4 + src_col / 2];
            uint8_t idx = (src_col & 1) ? (uint8_t)(byte & 0x0Fu) : (uint8_t)(byte >> 4);
            if (idx == 0)
                continue; /* transparent */
            int dst_off = py * CPS1_FB_WIDTH + px;
            fb[dst_off] = pal->colors[bank][idx];
            if (out_meta)
                out_meta[dst_off] = (uint8_t)(meta_prefix | (idx & CPS1_PIXEL_META_PEN_MASK));
        }
    }
}

CPS1_ITCM_TEXT
void cps1_blit_block_indexed(uint32_t base_subtile, unsigned sub, unsigned palette_bank,
                              const cps1_palette_t *pal, int dst_x, int dst_y,
                              int flip_x, int flip_y, cps1_tile_cache_t *cache,
                              const cps1_rom_t *rom, uint16_t *fb,
                              uint8_t *out_meta, uint8_t priority_group)
{
    for (unsigned qy = 0; qy < sub; qy++) {
        unsigned src_qy = flip_y ? (sub - 1u - qy) : qy;
        for (unsigned qx = 0; qx < sub; qx++) {
            unsigned src_qx = flip_x ? (sub - 1u - qx) : qx;
            uint32_t subtile = base_subtile + src_qy * sub + src_qx;
            const uint8_t *tile = cps1_tile_cache_fetch(cache, rom, subtile);
            if (!tile)
                continue;
            cps1_blit8x8_indexed(tile, palette_bank, pal,
                                  dst_x + (int)(qx * 8u), dst_y + (int)(qy * 8u),
                                  flip_x, flip_y, fb, out_meta, priority_group);
        }
    }
}

/*
 * Sprite footprint: (xblock+1)*(yblock+1) 16x16 units, each a 2x2 grid of
 * 8x8 sub-tiles at consecutive indices (base_unit*4 .. +3) -- same
 * decomposition cps1_bg.c uses for SCROLL2/3, not yet the real
 * CPS1_GFX_LAYOUT_16X16 raw decode (Phase 11). Block iteration and the x/y
 * coordinate wrap (& 0x1ff -- CPS-1's sprites live in a 512x512 internal
 * coordinate space, confirmed cps1_v.cpp:2761-2851) mirror MAME's
 * cps1_render_sprites DRAWSPRITE loop: screen position always progresses
 * left-to-right/top-to-bottom by block index, only the SOURCE unit
 * selection mirrors under flip (so the whole block's arrangement flips,
 * not just each unit's own pixels).
 */
void cps1_ppu_render(const cps1_oam_t *oam, const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                      const cps1_palette_t *pal, uint16_t *fb)
{
    for (uint32_t i = 0; i < oam->count; i++) {
        const cps1_oam_entry_t *s = &oam->sprites[i];
        if (!s->enabled)
            continue;

        unsigned nx = ((s->attr >> CPS1_OAM_ATTR_XBLOCK_SHIFT) & CPS1_OAM_ATTR_BLOCK_MASK) + 1u;
        unsigned ny = ((s->attr >> CPS1_OAM_ATTR_YBLOCK_SHIFT) & CPS1_OAM_ATTR_BLOCK_MASK) + 1u;
        int footprint_w = (int)(nx * 16u);
        int footprint_h = (int)(ny * 16u);
        if (s->x + footprint_w <= 0 || s->x >= CPS1_FB_WIDTH)
            continue;
        if (s->y + footprint_h <= 0 || s->y >= CPS1_FB_HEIGHT)
            continue;

        unsigned color = s->attr & CPS1_OAM_ATTR_COLOR_MASK;
        int flip_x = (s->attr & CPS1_OAM_ATTR_FLIP_X) != 0;
        int flip_y = (s->attr & CPS1_OAM_ATTR_FLIP_Y) != 0;
        uint32_t ux = (uint32_t)(int32_t)s->x;
        uint32_t uy = (uint32_t)(int32_t)s->y;

        for (unsigned nys = 0; nys < ny; nys++) {
            unsigned src_ny = flip_y ? (ny - 1u - nys) : nys;
            int sy = (int)((uy + nys * 16u) & 0x1FFu);
            for (unsigned nxs = 0; nxs < nx; nxs++) {
                unsigned src_nx = flip_x ? (nx - 1u - nxs) : nxs;
                int sx = (int)((ux + nxs * 16u) & 0x1FFu);
                uint32_t unit_base = (uint32_t)s->tile_index + (src_ny * nx + src_nx) * 4u;
                cps1_blit_block_indexed(unit_base, 2u, color, pal, sx, sy,
                                         flip_x, flip_y, cache, rom, fb, NULL, 0);
            }
        }
    }
}
