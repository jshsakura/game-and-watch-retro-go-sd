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

typedef struct {
    int16_t x, y;
    uint16_t tile_index;
    uint8_t attr;
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
    uint16_t tile_index;
    uint8_t valid;
    uint8_t pixels[CPS1_TILE_SIZE_BYTES];
} cps1_tile_cache_slot_t;

/* Slot count derived from sizeof(slot), not a naive 256KB/32B division --
 * per-slot metadata (last_used/tile_index/valid) is real RAM cost too, so
 * the array is sized to make its OWN total footprint land at 256KB,
 * instead of quietly costing more than the budgeted number. */
enum { CPS1_TILE_CACHE_BUDGET_BYTES = 256 * 1024 };
enum { CPS1_TILE_CACHE_SLOTS = CPS1_TILE_CACHE_BUDGET_BYTES / (int)sizeof(cps1_tile_cache_slot_t) };

typedef struct {
    cps1_tile_cache_slot_t slots[CPS1_TILE_CACHE_SLOTS];
    uint32_t clock;
    uint32_t hits;
    uint32_t misses;
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

/* Palette RAM skeleton: 32 banks x 16 colors, RGB565, index 0/bank is
 * transparent by convention (never written to the framebuffer). Real
 * CPS-1 palette RAM is bigger/laid out differently on hardware -- this is
 * just enough to make cps1_ppu_render() produce a real, checkable image. */
#define CPS1_PALETTE_BANKS  32
#define CPS1_PALETTE_COLORS 16

typedef struct {
    uint16_t colors[CPS1_PALETTE_BANKS][CPS1_PALETTE_COLORS];
} cps1_palette_t;

/* Renders every visible OAM sprite into `fb` (CPS1_FB_WIDTH x
 * CPS1_FB_HEIGHT, RGB565, NOT cleared by this function -- caller clears
 * first). Fetches each sprite's tile through the cache (a "Flash dump" on
 * miss), unpacks its 4bpp nibbles (high nibble = left pixel of each byte's
 * pixel pair), and looks up color via pal->colors[attr & 0x1F][index].
 * Index 0 is transparent and left unwritten. This is a straight 8x8
 * unscaled blit -- no scroll layers, no priority/Z-order, no scaling; see
 * docs/CPS1_ULTIMATE_PORTING_PLAN.md techniques 4/8 for what replaces it. */
void cps1_ppu_render(const cps1_oam_t *oam, const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                      const cps1_palette_t *pal, uint16_t *fb);
