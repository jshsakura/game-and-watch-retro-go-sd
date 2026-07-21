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
    if (cps1_rom_decode_tile(rom, tile_index, slot->pixels) != 0) {
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

void cps1_blit8x8_indexed(const uint8_t *tile4bpp, unsigned palette_bank,
                           const cps1_palette_t *pal, int dst_x, int dst_y,
                           int flip_x, int flip_y, uint16_t *fb,
                           uint8_t *out_meta, uint8_t priority_group)
{
    unsigned bank = palette_bank & (CPS1_PALETTE_BANKS - 1);
    uint8_t meta_prefix = (uint8_t)(CPS1_PIXEL_META_VALID |
                                     ((priority_group & CPS1_PIXEL_META_GROUP_MASK)
                                      << CPS1_PIXEL_META_GROUP_SHIFT));
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
