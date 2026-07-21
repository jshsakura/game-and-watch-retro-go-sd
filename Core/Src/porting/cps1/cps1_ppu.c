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
        /* Spatial pre-filter (technique 5): an 8x8 tile fully to the left/
         * above/right/below the viewport can never contribute a pixel, so
         * it never needs a cache slot at all. */
        if (s->x + 8 <= 0 || s->x >= CPS1_FB_WIDTH)
            continue;
        if (s->y + 8 <= 0 || s->y >= CPS1_FB_HEIGHT)
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
                           const cps1_palette_t *pal, int dst_x, int dst_y, uint16_t *fb)
{
    unsigned bank = palette_bank & (CPS1_PALETTE_BANKS - 1);
    for (int row = 0; row < 8; row++) {
        int py = dst_y + row;
        if (py < 0 || py >= CPS1_FB_HEIGHT)
            continue;
        for (int col = 0; col < 8; col++) {
            int px = dst_x + col;
            if (px < 0 || px >= CPS1_FB_WIDTH)
                continue;
            uint8_t byte = tile4bpp[row * 4 + col / 2];
            uint8_t idx = (col & 1) ? (uint8_t)(byte & 0x0Fu) : (uint8_t)(byte >> 4);
            if (idx == 0)
                continue; /* transparent */
            fb[py * CPS1_FB_WIDTH + px] = pal->colors[bank][idx];
        }
    }
}

void cps1_ppu_render(const cps1_oam_t *oam, const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                      const cps1_palette_t *pal, uint16_t *fb)
{
    for (uint32_t i = 0; i < oam->count; i++) {
        const cps1_oam_entry_t *s = &oam->sprites[i];
        if (!s->enabled)
            continue;
        if (s->x + 8 <= 0 || s->x >= CPS1_FB_WIDTH)
            continue;
        if (s->y + 8 <= 0 || s->y >= CPS1_FB_HEIGHT)
            continue;

        const uint8_t *tile = cps1_tile_cache_fetch(cache, rom, s->tile_index);
        if (!tile)
            continue;

        cps1_blit8x8_indexed(tile, s->attr, pal, s->x, s->y, fb);
    }
}
