/*
 * Standalone build-test for cps1_rom.c + cps1_ppu.c against synthetic data
 * (no real CPS-1 ROM exists yet). Proves: tile decode returns the right
 * bytes, OAM prescan culls off-screen sprites and passes on-screen ones,
 * and the direct-mapped cache evicts+re-decodes correctly on collision
 * instead of silently returning stale data.
 *
 *   ./build/cps1-ppu-selftest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps1_ppu.h"
#include "cps1_rom.h"

static int failures = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                          \
            failures++;                                                     \
        }                                                                    \
    } while (0)

/* Synthetic GFX ROM: tile N's 32 bytes are all == (N & 0xFF), so decoding
 * tile N and checking every byte == (N & 0xFF) proves the right offset was
 * read -- not just that some bytes came back. */
static uint8_t *make_synthetic_gfx(uint32_t tile_count)
{
    uint32_t size = tile_count * CPS1_TILE_SIZE_BYTES;
    uint8_t *buf = malloc(size);
    for (uint32_t t = 0; t < tile_count; t++)
        memset(buf + t * CPS1_TILE_SIZE_BYTES, (int)(t & 0xFF), CPS1_TILE_SIZE_BYTES);
    return buf;
}

static void test_rom_decode(const cps1_rom_t *rom)
{
    uint8_t out[CPS1_TILE_SIZE_BYTES];

    CHECK(cps1_rom_decode_tile(rom, 0, out) == 0, "tile 0 decode should succeed");
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        CHECK(out[i] == 0, "tile 0 byte %d should be 0x00, got 0x%02x", i, out[i]);

    CHECK(cps1_rom_decode_tile(rom, 5, out) == 0, "tile 5 decode should succeed");
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        CHECK(out[i] == 5, "tile 5 byte %d should be 0x05, got 0x%02x", i, out[i]);

    uint32_t last_valid = rom->gfx.size / CPS1_TILE_SIZE_BYTES - 1;
    CHECK(cps1_rom_decode_tile(rom, last_valid + 1, out) != 0,
          "decoding past the end of gfx should fail");
}

static void test_oam_prescan(void)
{
    cps1_oam_t oam;
    memset(&oam, 0, sizeof(oam));

    /* 0: fully on-screen */
    oam.sprites[0] = (cps1_oam_entry_t){.x = 100, .y = 100, .tile_index = 10, .enabled = 1};
    /* 1: off-screen left (x+8 <= 0) */
    oam.sprites[1] = (cps1_oam_entry_t){.x = -8, .y = 50, .tile_index = 11, .enabled = 1};
    /* 2: off-screen right (x >= CPS1_FB_WIDTH) */
    oam.sprites[2] = (cps1_oam_entry_t){.x = CPS1_FB_WIDTH, .y = 50, .tile_index = 12, .enabled = 1};
    /* 3: off-screen above */
    oam.sprites[3] = (cps1_oam_entry_t){.x = 50, .y = -8, .tile_index = 13, .enabled = 1};
    /* 4: off-screen below */
    oam.sprites[4] = (cps1_oam_entry_t){.x = 50, .y = CPS1_FB_HEIGHT, .tile_index = 14, .enabled = 1};
    /* 5: on-screen but disabled */
    oam.sprites[5] = (cps1_oam_entry_t){.x = 20, .y = 20, .tile_index = 15, .enabled = 0};
    /* 6: straddling the left edge -- must still count as visible */
    oam.sprites[6] = (cps1_oam_entry_t){.x = -4, .y = 20, .tile_index = 16, .enabled = 1};
    oam.count = 7;

    uint16_t visible[CPS1_OAM_MAX_SPRITES];
    uint32_t n = cps1_oam_prescan(&oam, visible, CPS1_OAM_MAX_SPRITES);

    CHECK(n == 2, "expected 2 visible sprites (10, 16), got %u", n);
    if (n == 2) {
        CHECK(visible[0] == 10, "first visible tile should be 10, got %u", visible[0]);
        CHECK(visible[1] == 16, "second visible tile should be 16 (edge straddle), got %u", visible[1]);
    }

    /* max_out clamps output even if more sprites are visible. */
    oam.sprites[5] = (cps1_oam_entry_t){.x = 5, .y = 5, .tile_index = 99, .enabled = 1};
    n = cps1_oam_prescan(&oam, visible, 1);
    CHECK(n == 1, "max_out=1 should clamp to 1, got %u", n);
}

static void test_tile_cache(const cps1_rom_t *rom)
{
    cps1_tile_cache_t cache;
    cps1_tile_cache_reset(&cache);

    const uint8_t *p = cps1_tile_cache_fetch(&cache, rom, 3);
    CHECK(p != NULL, "fetch tile 3 should succeed");
    CHECK(cache.misses == 1 && cache.hits == 0, "first fetch should be a miss (misses=%u hits=%u)",
          cache.misses, cache.hits);
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        CHECK(p[i] == 3, "cached tile 3 byte %d should be 0x03, got 0x%02x", i, p[i]);

    p = cps1_tile_cache_fetch(&cache, rom, 3);
    CHECK(cache.misses == 1 && cache.hits == 1, "re-fetching tile 3 should hit (misses=%u hits=%u)",
          cache.misses, cache.hits);

    /* Force a collision: tile (3 + CPS1_TILE_CACHE_SLOTS) maps to the same
     * direct-mapped slot as tile 3 and must evict it. */
    uint32_t colliding_tile = 3 + (uint32_t)CPS1_TILE_CACHE_SLOTS;
    p = cps1_tile_cache_fetch(&cache, rom, colliding_tile);
    CHECK(p != NULL, "fetch of colliding tile should succeed");
    CHECK(cache.misses == 2, "colliding tile should be a miss (misses=%u)", cache.misses);
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        CHECK(p[i] == (uint8_t)(colliding_tile & 0xFF),
              "colliding tile byte %d should be 0x%02x, got 0x%02x",
              i, (uint8_t)(colliding_tile & 0xFF), p[i]);

    /* Tile 3 was evicted -- fetching it again must be a fresh miss, not a
     * stale/incorrect hit. */
    p = cps1_tile_cache_fetch(&cache, rom, 3);
    CHECK(cache.misses == 3, "re-fetching evicted tile 3 should miss again (misses=%u)", cache.misses);
    for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        CHECK(p[i] == 3, "re-decoded tile 3 byte %d should be 0x03, got 0x%02x", i, p[i]);
}

int main(void)
{
    uint32_t tile_count = (uint32_t)CPS1_TILE_CACHE_SLOTS + 16;
    uint8_t *gfx = make_synthetic_gfx(tile_count);
    uint8_t prg_dummy[1] = {0};

    cps1_rom_t rom;
    cps1_rom_region_t prg_region = {prg_dummy, sizeof(prg_dummy)};
    cps1_rom_region_t gfx_region = {gfx, tile_count * CPS1_TILE_SIZE_BYTES};
    cps1_rom_region_t empty = {0};
    CHECK(cps1_rom_attach(&rom, prg_region, gfx_region, empty, empty) == 0,
          "cps1_rom_attach should succeed with prg+gfx present");

    test_rom_decode(&rom);
    test_oam_prescan();
    test_tile_cache(&rom);

    free(gfx);

    printf("[cps1-ppu-selftest] CPS1_TILE_CACHE_SLOTS=%d sizeof(cache)=%zu bytes\n",
           (int)CPS1_TILE_CACHE_SLOTS, sizeof(cps1_tile_cache_t));

    if (failures) {
        fprintf(stderr, "[cps1-ppu-selftest] FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-ppu-selftest] OK\n");
    return 0;
}
