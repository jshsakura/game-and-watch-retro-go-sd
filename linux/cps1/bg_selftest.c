/*
 * Build-test for cps1_bg.c: renders all three scroll layers into their own
 * buffers (as LTDC's two hardware layers + the CPU top buffer would be)
 * and checks the host compositor picks top > middle > bottom correctly,
 * and shows the layer below through wherever a layer is absent/transparent
 * -- not just that nothing crashed.
 *
 *   ./build/cps1-bg-selftest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cps1_bg.h"
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

/* Tile T's bytes are all (T & 0xFF), same convention as ppu_selftest.c.
 * Base tile 0x11 (both nibbles = 1) makes every pixel of its FIRST 8x8
 * sub-tile decode to index 1 -- exactly what every checked pixel below
 * lands on, regardless of a layer's sub-tile count. */
#define BASE_TILE 0x11u

static uint8_t *make_synthetic_gfx(uint32_t tile_count)
{
    uint32_t size = tile_count * CPS1_TILE_SIZE_BYTES;
    uint8_t *buf = malloc(size);
    for (uint32_t t = 0; t < tile_count; t++)
        memset(buf + t * CPS1_TILE_SIZE_BYTES, (int)(t & 0xFF), CPS1_TILE_SIZE_BYTES);
    return buf;
}

static void enable_cell(cps1_bg_layer_t *layer, int col, int row, unsigned palette_bank)
{
    cps1_bg_cell_t *c = &layer->cells[row * CPS1_BG_MAP_W + col];
    c->tile_index = BASE_TILE;
    c->palette = (uint8_t)palette_bank;
    c->enabled = 1;
}

int main(void)
{
    uint32_t tile_count = BASE_TILE + 16; /* covers SCROLL3's 4x4=16 sub-tiles from BASE_TILE */
    uint8_t *gfx = make_synthetic_gfx(tile_count);
    uint8_t prg_dummy[1] = {0};

    cps1_rom_t rom;
    cps1_rom_region_t prg = {prg_dummy, sizeof(prg_dummy)};
    cps1_rom_region_t gfx_region = {gfx, tile_count * CPS1_TILE_SIZE_BYTES};
    cps1_rom_region_t empty = {0};
    CHECK(cps1_rom_attach(&rom, prg, gfx_region, empty, empty) == 0, "rom attach should succeed");

    cps1_palette_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.colors[0][1] = 0xF800; /* bottom (SCROLL1)  -> red    */
    pal.colors[1][1] = 0xFFE0; /* middle (SCROLL2)  -> yellow */
    pal.colors[2][1] = 0x07FF; /* top    (SCROLL3)  -> cyan   */

    cps1_bg_state_t bg;
    cps1_bg_reset(&bg);

    /* SCROLL1 (8x8 cells): (0,0)->px(0-7,0-7), (5,5)->px(40-47,40-47) [bottom-only],
     * (6,6)->px(48-55,48-55) [bottom+middle]. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 0, 0, 0);
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 5, 5, 0);
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 6, 6, 0);

    /* SCROLL2 (16x16 cells): (0,0)->px(0-15,0-15) [3-way overlap],
     * (3,3)->px(48-63,48-63) [bottom+middle]. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL2], 0, 0, 1);
    enable_cell(&bg.layers[CPS1_BG_SCROLL2], 3, 3, 1);

    /* SCROLL3 (32x32 cells): (0,0)->px(0-31,0-31) only [3-way overlap]. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL3], 0, 0, 2);

    cps1_tile_cache_t cache;
    cps1_tile_cache_reset(&cache);

    static uint16_t bottom_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t middle_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t top_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t out_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    memset(bottom_fb, 0, sizeof(bottom_fb));
    memset(middle_fb, 0, sizeof(middle_fb));
    memset(top_fb, 0, sizeof(top_fb));

    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL1], CPS1_BG_SCROLL1, &rom, &cache, &pal, bottom_fb);
    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL2], CPS1_BG_SCROLL2, &rom, &cache, &pal, middle_fb);
    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL3], CPS1_BG_SCROLL3, &rom, &cache, &pal, top_fb);

    /* Sanity: each layer's own buffer has the expected raw color before
     * compositing (isolates a layer-render bug from a compositor bug). */
    CHECK(bottom_fb[4 * CPS1_FB_WIDTH + 4] == 0xF800, "bottom raw px(4,4) should be red");
    CHECK(middle_fb[4 * CPS1_FB_WIDTH + 4] == 0xFFE0, "middle raw px(4,4) should be yellow");
    CHECK(top_fb[4 * CPS1_FB_WIDTH + 4] == 0x07FF, "top raw px(4,4) should be cyan");
    CHECK(bottom_fb[44 * CPS1_FB_WIDTH + 44] == 0xF800, "bottom raw px(44,44) should be red");
    CHECK(middle_fb[44 * CPS1_FB_WIDTH + 44] == 0, "middle raw px(44,44) should be transparent");
    CHECK(top_fb[44 * CPS1_FB_WIDTH + 44] == 0, "top raw px(44,44) should be transparent");
    CHECK(bottom_fb[52 * CPS1_FB_WIDTH + 52] == 0xF800, "bottom raw px(52,52) should be red");
    CHECK(middle_fb[52 * CPS1_FB_WIDTH + 52] == 0xFFE0, "middle raw px(52,52) should be yellow");
    CHECK(top_fb[52 * CPS1_FB_WIDTH + 52] == 0, "top raw px(52,52) should be transparent");

    cps1_compositor_blend(bottom_fb, middle_fb, top_fb, out_fb);

    uint16_t p_overlap = out_fb[4 * CPS1_FB_WIDTH + 4];
    uint16_t p_bottom_only = out_fb[44 * CPS1_FB_WIDTH + 44];
    uint16_t p_bottom_middle = out_fb[52 * CPS1_FB_WIDTH + 52];

    printf("[cps1-bg-selftest] px(4,4)=%04x px(44,44)=%04x px(52,52)=%04x\n",
           p_overlap, p_bottom_only, p_bottom_middle);

    CHECK(p_overlap == 0x07FF, "3-way overlap (4,4) should composite to top's cyan, got %04x", p_overlap);
    CHECK(p_bottom_only == 0xF800, "bottom-only (44,44) should composite to red, got %04x", p_bottom_only);
    CHECK(p_bottom_middle == 0xFFE0, "bottom+middle (52,52) should composite to yellow, got %04x", p_bottom_middle);

    /* A pixel no layer touches must stay 0 (backdrop), not leak stale data. */
    CHECK(out_fb[200 * CPS1_FB_WIDTH + 200] == 0, "untouched px(200,200) should be backdrop 0");

    free(gfx);

    if (failures) {
        fprintf(stderr, "[cps1-bg-selftest] FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-bg-selftest] OK\n");
    return 0;
}
