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

/*
 * cps1_rom_decode_tile_planar against the REAL, MAME-confirmed bitplane
 * layout (docs/CPS1_MAME_ALIGNMENT.md section 1) -- separate from
 * test_rom_decode above, which tests the OLD flat/pre-packed decoder
 * against already-packed synthetic data (still valid for its own purpose,
 * unchanged). This is the Phase 8 correctness gate: hand-derive the
 * expected packed-nibble output from the confirmed spec, independent of
 * the implementation under test, the same way every other selftest in
 * this initiative verifies against a hand trace rather than the code's
 * own formula.
 *
 * Real layout: each row is 8 raw bytes -- byte0=plane-index3(pixel LSB),
 * byte1=plane-index2, byte2=plane-index1, byte3=plane-index0(pixel MSB)
 * for the LEFT half-tile; bytes 4-7 are the RIGHT half-tile (unused by
 * CPS1_GFX_LAYOUT_8X8_LEFT). Bit 0 (MSB, 0x80) of each byte = column 0.
 */
static void test_rom_decode_planar(void)
{
    uint8_t gfx[128]; /* 2 left-half 8x8 tiles' worth (64 bytes each) */
    memset(gfx, 0, sizeof(gfx));

    /* Tile 0, row 0: set bit0 (col 0) in byte0 (LSB plane), byte1, and
     * byte3 (MSB plane); leave byte2 clear -- expected pixel(0,0) = 0b1011
     * = 0xB (bit3=1 MSB-plane, bit2=0, bit1=1, bit0=1 LSB-plane). */
    gfx[0] = 0x80;
    gfx[1] = 0x80;
    gfx[2] = 0x00;
    gfx[3] = 0x80;

    /* Tile 1 (bytes 64-127), row 3 (bytes 64+24..64+27), col 7 (bit_in_byte
     * 7 = LSB 0x01, since col directly equals bit_in_byte for this
     * layout's byte-aligned row/plane bases): set only byte 64+24+1
     * (planeoffset[2]=8 -> byte offset 1 within the row => plane-index 2,
     * pixel bit1) -- expected pixel(row3,col7) = 0b0010 = 0x2, everything
     * else in tile 1 zero. */
    gfx[64 + 3 * 8 + 1] = 0x01;

    cps1_rom_t rom;
    uint8_t prg_dummy[1] = {0};
    cps1_rom_region_t prg_region = {prg_dummy, sizeof(prg_dummy)};
    cps1_rom_region_t gfx_region = {gfx, sizeof(gfx)};
    cps1_rom_region_t empty = {0};
    CHECK(cps1_rom_attach(&rom, prg_region, gfx_region, empty, empty) == 0,
          "planar rom attach should succeed");

    uint8_t out[CPS1_TILE_SIZE_BYTES];

    memset(out, 0xFF, sizeof(out));
    CHECK(cps1_rom_decode_tile_planar(&rom, &CPS1_GFX_LAYOUT_8X8_LEFT, 0, out) == 0,
          "planar tile 0 decode should succeed");
    CHECK((out[0] >> 4) == 0xB, "planar tile0 pixel(0,0) should be 0xB, got 0x%x", out[0] >> 4);
    {
        int rest_zero = 1;
        for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++) {
            uint8_t v = out[i];
            if (i == 0) v &= 0x0F; /* pixel(0,0) already checked above */
            if (v != 0) rest_zero = 0;
        }
        CHECK(rest_zero, "planar tile0's only nonzero pixel should be (0,0)");
    }

    memset(out, 0xFF, sizeof(out));
    CHECK(cps1_rom_decode_tile_planar(&rom, &CPS1_GFX_LAYOUT_8X8_LEFT, 1, out) == 0,
          "planar tile 1 decode should succeed");
    /* pixel(row3,col7) is byte(3*4 + 7/2)=byte15, low nibble (col7 odd) */
    CHECK((out[15] & 0x0F) == 0x2, "planar tile1 pixel(3,7) should be 0x2, got 0x%x", out[15] & 0x0F);
    {
        int rest_zero = 1;
        for (int i = 0; i < CPS1_TILE_SIZE_BYTES; i++) {
            uint8_t v = out[i];
            if (i == 15) v &= 0xF0; /* pixel(3,7) already checked above */
            if (v != 0) rest_zero = 0;
        }
        CHECK(rest_zero, "planar tile1's only nonzero pixel should be (3,7)");
    }

    /* Out-of-range tile must fail cleanly, not read past gfx.size. */
    CHECK(cps1_rom_decode_tile_planar(&rom, &CPS1_GFX_LAYOUT_8X8_LEFT, 2, out) != 0,
          "decoding a 3rd tile past the 128-byte gfx region should fail");

    /* 16x16/32x32 layouts: not yet wired into the live sub-tile cache
     * (docs/CPS1_MAME_ALIGNMENT.md section 8), but the confirmed layout
     * data itself must decode without crashing/reading out of bounds
     * against a suitably-sized region. */
    uint8_t big[4096];
    memset(big, 0xAA, sizeof(big));
    cps1_rom_t rom_big;
    cps1_rom_region_t big_region = {big, sizeof(big)};
    CHECK(cps1_rom_attach(&rom_big, prg_region, big_region, empty, empty) == 0,
          "big planar rom attach should succeed");
    uint8_t out16[128], out32[512];
    CHECK(cps1_rom_decode_tile_planar(&rom_big, &CPS1_GFX_LAYOUT_16X16, 0, out16) == 0,
          "16x16 layout should decode without crashing");
    CHECK(cps1_rom_decode_tile_planar(&rom_big, &CPS1_GFX_LAYOUT_32X32, 0, out32) == 0,
          "32x32 layout should decode without crashing");
}

/* Phase 10: sprite base footprint is 16x16, not 8x8 (docs/CPS1_MAME_
 * ALIGNMENT.md section 5) -- every off-screen/straddle coordinate below is
 * chosen relative to a 16px footprint, and sprite 6 specifically uses a
 * position (-8) that the OLD 8px-footprint cull would have DROPPED
 * (-8+8<=0) but the correct 16px footprint must keep VISIBLE (-8+16>0),
 * so this regresses if the footprint size is ever wrongly shrunk back. */
static void test_oam_prescan(void)
{
    cps1_oam_t oam;
    memset(&oam, 0, sizeof(oam));

    /* 0: fully on-screen */
    oam.sprites[0] = (cps1_oam_entry_t){.x = 100, .y = 100, .tile_index = 10, .enabled = 1};
    /* 1: off-screen left (x+16 <= 0) */
    oam.sprites[1] = (cps1_oam_entry_t){.x = -16, .y = 50, .tile_index = 11, .enabled = 1};
    /* 2: off-screen right (x >= CPS1_FB_WIDTH) */
    oam.sprites[2] = (cps1_oam_entry_t){.x = CPS1_FB_WIDTH, .y = 50, .tile_index = 12, .enabled = 1};
    /* 3: off-screen above (y+16 <= 0) */
    oam.sprites[3] = (cps1_oam_entry_t){.x = 50, .y = -16, .tile_index = 13, .enabled = 1};
    /* 4: off-screen below */
    oam.sprites[4] = (cps1_oam_entry_t){.x = 50, .y = CPS1_FB_HEIGHT, .tile_index = 14, .enabled = 1};
    /* 5: on-screen but disabled */
    oam.sprites[5] = (cps1_oam_entry_t){.x = 20, .y = 20, .tile_index = 15, .enabled = 0};
    /* 6: straddling the left edge with the WIDER 16px footprint -- must
     * still count as visible (would have been wrongly culled at 8px). */
    oam.sprites[6] = (cps1_oam_entry_t){.x = -8, .y = 20, .tile_index = 16, .enabled = 1};
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

/*
 * Multi-tile block sprite + flip -- docs/CPS1_MAME_ALIGNMENT.md section 5.
 * A 2x1-block (nx=2,ny=1) sprite with flip_x=1 must draw its SOURCE units
 * mirrored (unit 1's content on the LEFT screen slot, unit 0's on the
 * RIGHT), proving cps1_ppu_render's block iteration -- not just that a
 * single 16x16 unit renders. Per-pixel mirroring WITHIN one 8x8 sub-tile
 * is exercised separately by cps1-bg-selftest's asymmetric FLIP_TILE case
 * (same shared cps1_blit8x8_indexed/cps1_blit_block_indexed code path),
 * so this test uses solid-color units where inner-pixel flip wouldn't be
 * observable anyway.
 */
static void test_sprite_block_flip(void)
{
    /* unit 0 = tile_index+0..+3 (all pen 1, "0x11"); unit 1 = +4..+7 (all
     * pen 2, "0x22") -- each unit is 4 consecutive 8x8 sub-tiles (2x2). */
    enum { BASE = 40, TILE_N = BASE + 8 };
    uint8_t *gfx = malloc((uint32_t)TILE_N * CPS1_TILE_SIZE_BYTES);
    memset(gfx, 0, (uint32_t)TILE_N * CPS1_TILE_SIZE_BYTES);
    for (int t = 0; t < 4; t++)
        memset(gfx + (BASE + t) * CPS1_TILE_SIZE_BYTES, 0x11, CPS1_TILE_SIZE_BYTES);
    for (int t = 4; t < 8; t++)
        memset(gfx + (BASE + t) * CPS1_TILE_SIZE_BYTES, 0x22, CPS1_TILE_SIZE_BYTES);

    cps1_rom_t rom;
    uint8_t prg_dummy[1] = {0};
    cps1_rom_region_t prg = {prg_dummy, sizeof(prg_dummy)};
    cps1_rom_region_t gfx_region = {gfx, (uint32_t)TILE_N * CPS1_TILE_SIZE_BYTES};
    cps1_rom_region_t empty = {0};
    CHECK(cps1_rom_attach(&rom, prg, gfx_region, empty, empty) == 0, "block-flip rom attach should succeed");

    cps1_palette_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.colors[0][1] = 0xF800; /* red   -- unit 0's pen */
    pal.colors[0][2] = 0x07E0; /* green -- unit 1's pen */

    cps1_oam_t oam;
    memset(&oam, 0, sizeof(oam));
    oam.count = 1;
    oam.sprites[0] = (cps1_oam_entry_t){
        .x = 0, .y = 0, .tile_index = BASE,
        .attr = (1u << CPS1_OAM_ATTR_XBLOCK_SHIFT) /* nx-1=1 -> nx=2 */ | CPS1_OAM_ATTR_FLIP_X,
        .enabled = 1,
    };

    cps1_tile_cache_t cache;
    cps1_tile_cache_reset(&cache);
    static uint16_t fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    memset(fb, 0, sizeof(fb));
    cps1_ppu_render(&oam, &rom, &cache, &pal, fb);

    CHECK(fb[4 * CPS1_FB_WIDTH + 4] == 0x07E0,
          "flip_x block: left 16px slot should show unit 1's green, got %04x", fb[4 * CPS1_FB_WIDTH + 4]);
    CHECK(fb[4 * CPS1_FB_WIDTH + 20] == 0xF800,
          "flip_x block: right 16px slot should show unit 0's red, got %04x", fb[4 * CPS1_FB_WIDTH + 20]);

    free(gfx);
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
    test_rom_decode_planar();
    test_oam_prescan();
    test_tile_cache(&rom);
    test_sprite_block_flip();

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
