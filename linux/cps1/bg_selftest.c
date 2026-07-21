/*
 * Build-test for cps1_bg.c: renders all three scroll layers into their own
 * buffers (as LTDC's two hardware layers + the CPU top buffer would be)
 * and checks the host compositor picks the correct final pixel per priority
 * group + CPS-B mask, per-layer palette offset is real (not accidental),
 * X/Y tile flip actually mirrors pixels, and the tilemap bit-swizzle
 * addressing round-trips -- not just that nothing crashed.
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

/* Tile 0x11's bytes are all 0x11 (both nibbles = 1) -- every pixel of its
 * first 8x8 sub-tile decodes to pen index 1, same convention every other
 * cps1 selftest in this initiative uses. */
#define BASE_TILE 0x11u

/* Tile 0x22: asymmetric so flip is actually observable -- row0 is all pen
 * 2, every other row all pen 3, and within each row col0 differs from the
 * rest so BOTH x and y flip are independently detectable at a single
 * corner pixel: (row0,col0) = 2, everywhere else in the tile = 3. */
#define FLIP_TILE 0x22u

static uint8_t *make_synthetic_gfx(uint32_t tile_count)
{
    uint32_t size = tile_count * CPS1_TILE_SIZE_BYTES;
    uint8_t *buf = malloc(size);
    for (uint32_t t = 0; t < tile_count; t++)
        memset(buf + t * CPS1_TILE_SIZE_BYTES, (int)(t & 0xFF), CPS1_TILE_SIZE_BYTES);

    /* Overwrite FLIP_TILE with the asymmetric pattern described above.
     * Packed 4bpp: byte(row*4+col/2), high nibble = even col, low = odd. */
    uint8_t *flip_tile = buf + FLIP_TILE * CPS1_TILE_SIZE_BYTES;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint8_t pen = (row == 0 && col == 0) ? 2u : 3u;
            uint8_t *b = &flip_tile[row * 4 + col / 2];
            if (col & 1) *b = (uint8_t)((*b & 0xF0u) | pen);
            else         *b = (uint8_t)((*b & 0x0Fu) | (pen << 4));
        }
    }
    return buf;
}

static void enable_cell(cps1_bg_layer_t *layer, int col, int row, unsigned code,
                         unsigned color, unsigned flip_x, unsigned flip_y, unsigned prio)
{
    cps1_bg_cell_t *c = &layer->cells[row * CPS1_BG_MAP_W + col];
    c->code = (uint16_t)code;
    c->attr = (uint16_t)((color & CPS1_BG_ATTR_COLOR_MASK) |
                          (flip_x ? CPS1_BG_ATTR_FLIP_X : 0) |
                          (flip_y ? CPS1_BG_ATTR_FLIP_Y : 0) |
                          ((prio & CPS1_BG_ATTR_PRIORITY_MASK) << CPS1_BG_ATTR_PRIORITY_SHIFT));
}

/* Every logical (col,row) must round-trip through the bit-swizzle forward/
 * inverse pair exactly, for all three layers -- docs/CPS1_MAME_ALIGNMENT.md
 * section 6. Covers row/col values that exercise every masked bit range
 * (0, small, and near the 64-wide boundary), not just (0,0). */
static void test_swizzle_round_trip(void)
{
    static const unsigned sample[] = { 0, 1, 5, 17, 31, 32, 42, 63 };
    for (unsigned layer = 0; layer < CPS1_BG_LAYER_COUNT; layer++) {
        for (unsigned ci = 0; ci < sizeof(sample) / sizeof(sample[0]); ci++) {
            for (unsigned ri = 0; ri < sizeof(sample) / sizeof(sample[0]); ri++) {
                unsigned col = sample[ci], row = sample[ri];
                uint32_t off = cps1_bg_swizzle_col_row_to_offset(layer, col, row);
                unsigned c2 = 999, r2 = 999;
                cps1_bg_swizzle_offset_to_col_row(layer, off, &c2, &r2);
                CHECK(c2 == col && r2 == row,
                            "layer %u (col=%u,row=%u) -> offset %u -> (col=%u,row=%u)",
                            layer, col, row, off, c2, r2);
            }
        }
    }
}

int main(void)
{
    uint32_t tile_count = FLIP_TILE + 1; /* covers SCROLL3's base+15 sub-tiles AND FLIP_TILE itself */
    uint8_t *gfx = make_synthetic_gfx(tile_count);
    uint8_t prg_dummy[1] = {0};

    cps1_rom_t rom;
    cps1_rom_region_t prg = {prg_dummy, sizeof(prg_dummy)};
    cps1_rom_region_t gfx_region = {gfx, tile_count * CPS1_TILE_SIZE_BYTES};
    cps1_rom_region_t empty = {0};
    CHECK(cps1_rom_attach(&rom, prg, gfx_region, empty, empty) == 0, "rom attach should succeed");

    test_swizzle_round_trip();

    /* Palette: color field = 1 on every cell below -- if per-layer
     * palette offset (+0x20/+0x40/+0x60) is wired correctly, each layer
     * reads a DIFFERENT bank despite the same raw color field. Bank 1
     * itself (no offset) is poisoned with a sentinel so a regression to
     * "raw color used directly" is caught, not silently matched. */
    cps1_palette_t pal;
    memset(&pal, 0, sizeof(pal));
    pal.colors[1][1] = 0xDEAD & 0xFFFFu; /* sentinel: must NOT appear in output */
    pal.colors[cps1_bg_layer_palette_offset(CPS1_BG_SCROLL1) + 1][1] = 0xF800; /* red */
    pal.colors[cps1_bg_layer_palette_offset(CPS1_BG_SCROLL2) + 1][1] = 0xFFE0; /* yellow */
    pal.colors[cps1_bg_layer_palette_offset(CPS1_BG_SCROLL3) + 1][1] = 0x07FF; /* cyan */
    /* pen indices 2/3 for the flip test, on SCROLL1's own (offset+1) bank. */
    pal.colors[cps1_bg_layer_palette_offset(CPS1_BG_SCROLL1) + 1][2] = 0x001F; /* blue  */
    pal.colors[cps1_bg_layer_palette_offset(CPS1_BG_SCROLL1) + 1][3] = 0x0400; /* dark green */

    cps1_bg_state_t bg;
    cps1_bg_reset(&bg);

    /* SCROLL1 (8x8 cells): (0,0)->px(0-7,0-7), (5,5)->px(40-47,40-47) [bottom-only],
     * (6,6)->px(48-55,48-55) [bottom+middle]. color=1 on all three. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 0, 0, BASE_TILE, 1, 0, 0, 0);
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 5, 5, BASE_TILE, 1, 0, 0, 0);
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 6, 6, BASE_TILE, 1, 0, 0, 0);

    /* SCROLL2 (16x16 cells): (0,0)->px(0-15,0-15) [3-way overlap],
     * (3,3)->px(48-63,48-63) [bottom+middle]. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL2], 0, 0, BASE_TILE, 1, 0, 0, 0);
    enable_cell(&bg.layers[CPS1_BG_SCROLL2], 3, 3, BASE_TILE, 1, 0, 0, 0);

    /* SCROLL3 (32x32 cells): (0,0)->px(0-31,0-31) only [3-way overlap]. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL3], 0, 0, BASE_TILE, 1, 0, 0, 0);

    /* Flip test cell: SCROLL1 (10,0) with flip_x=flip_y=1. Unflipped,
     * FLIP_TILE's (row0,col0) = pen2, everywhere else = pen3. Flipped both
     * ways, pen2 must appear at the tile's BOTTOM-RIGHT corner instead. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL1], 10, 0, FLIP_TILE, 1, 1, 1, 0);

    cps1_tile_cache_t cache;
    cps1_tile_cache_reset(&cache);

    static uint16_t bottom_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint8_t  bottom_meta[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t middle_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint8_t  middle_meta[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t top_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint8_t  top_meta[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t sprite_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    static uint16_t out_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    memset(bottom_fb, 0, sizeof(bottom_fb)); memset(bottom_meta, 0, sizeof(bottom_meta));
    memset(middle_fb, 0, sizeof(middle_fb)); memset(middle_meta, 0, sizeof(middle_meta));
    memset(top_fb, 0, sizeof(top_fb));       memset(top_meta, 0, sizeof(top_meta));
    memset(sprite_fb, 0, sizeof(sprite_fb));

    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL1], CPS1_BG_SCROLL1, &rom, &cache, &pal, bottom_fb, bottom_meta);
    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL2], CPS1_BG_SCROLL2, &rom, &cache, &pal, middle_fb, middle_meta);
    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL3], CPS1_BG_SCROLL3, &rom, &cache, &pal, top_fb, top_meta);

    /* Sanity: each layer's own buffer has the expected (layer-offset)
     * color before compositing (isolates a layer-render bug from a
     * compositor bug), proving per-layer palette offset is real. */
    CHECK(bottom_fb[4 * CPS1_FB_WIDTH + 4] == 0xF800, "bottom raw px(4,4) should be red (layer-offset bank)");
    CHECK(middle_fb[4 * CPS1_FB_WIDTH + 4] == 0xFFE0, "middle raw px(4,4) should be yellow (layer-offset bank)");
    CHECK(top_fb[4 * CPS1_FB_WIDTH + 4] == 0x07FF, "top raw px(4,4) should be cyan (layer-offset bank)");
    CHECK(bottom_fb[44 * CPS1_FB_WIDTH + 44] == 0xF800, "bottom raw px(44,44) should be red");
    CHECK(middle_fb[44 * CPS1_FB_WIDTH + 44] == 0, "middle raw px(44,44) should be transparent");
    CHECK(top_fb[44 * CPS1_FB_WIDTH + 44] == 0, "top raw px(44,44) should be transparent");
    CHECK(bottom_fb[52 * CPS1_FB_WIDTH + 52] == 0xF800, "bottom raw px(52,52) should be red");
    CHECK(middle_fb[52 * CPS1_FB_WIDTH + 52] == 0xFFE0, "middle raw px(52,52) should be yellow");
    CHECK(top_fb[52 * CPS1_FB_WIDTH + 52] == 0, "top raw px(52,52) should be transparent");

    /* Flip: unflipped corner (row0,col0) at screen (80,0) must NOT be
     * pen2's blue; the MIRRORED corner (row7,col7) at screen (87,7) must
     * be blue instead. */
    CHECK(bottom_fb[0 * CPS1_FB_WIDTH + 80] == 0x0400,
                "flip: unflipped top-left screen slot should show pen3 (dark green), got %04x",
                bottom_fb[0 * CPS1_FB_WIDTH + 80]);
    CHECK(bottom_fb[7 * CPS1_FB_WIDTH + 87] == 0x001F,
                "flip: bottom-right screen slot should show pen2 (blue) after x+y flip, got %04x",
                bottom_fb[7 * CPS1_FB_WIDTH + 87]);

    cps1_priority_masks_t masks;
    memset(&masks, 0, sizeof(masks));
    const uint16_t *colors3[CPS1_BG_LAYER_COUNT] = { bottom_fb, middle_fb, top_fb };
    const uint8_t *metas3[CPS1_BG_LAYER_COUNT] = { bottom_meta, middle_meta, top_meta };

    /* No sprite anywhere yet -- compositing must reduce to plain BG
     * layering (highest opaque layer wins), same visual result the old
     * bottom<middle<top compositor gave for these same positions. */
    cps1_compositor_blend_priority(colors3, metas3, CPS1_BG_LAYER_COUNT, sprite_fb, &masks, out_fb);
    uint16_t p_overlap = out_fb[4 * CPS1_FB_WIDTH + 4];
    uint16_t p_bottom_only = out_fb[44 * CPS1_FB_WIDTH + 44];
    uint16_t p_bottom_middle = out_fb[52 * CPS1_FB_WIDTH + 52];
    printf("[cps1-bg-selftest] px(4,4)=%04x px(44,44)=%04x px(52,52)=%04x\n",
           p_overlap, p_bottom_only, p_bottom_middle);
    CHECK(p_overlap == 0x07FF, "3-way overlap (4,4) should composite to top's cyan, got %04x", p_overlap);
    CHECK(p_bottom_only == 0xF800, "bottom-only (44,44) should composite to red, got %04x", p_bottom_only);
    CHECK(p_bottom_middle == 0xFFE0, "bottom+middle (52,52) should composite to yellow, got %04x", p_bottom_middle);
    CHECK(out_fb[200 * CPS1_FB_WIDTH + 200] == 0, "untouched px(200,200) should be backdrop 0");

    /* Priority punch-through: put a SCROLL3 pixel with priority_group=2
     * under a fully-opaque sprite. With masks[2] pen-3 bit CLEAR, the
     * sprite (drawn on top of all BG by default) must win. With the bit
     * SET, that exact BG pixel must punch back over the sprite instead. */
    enable_cell(&bg.layers[CPS1_BG_SCROLL3], 1, 1, BASE_TILE, 1, 0, 0, 2); /* pen 1, group 2 */
    memset(top_fb, 0, sizeof(top_fb)); memset(top_meta, 0, sizeof(top_meta));
    cps1_bg_render_layer(&bg.layers[CPS1_BG_SCROLL3], CPS1_BG_SCROLL3, &rom, &cache, &pal, top_fb, top_meta);
    int px = 1 * 32 + 4, py = 1 * 32 + 4; /* inside cell (1,1)'s 32x32 footprint */
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++)
        sprite_fb[i] = 0xFFFF; /* fully opaque, non-zero everywhere */

    const uint16_t *colors1[1] = { top_fb };
    const uint8_t *metas1[1] = { top_meta };
    cps1_compositor_blend_priority(colors1, metas1, 1, sprite_fb, &masks, out_fb);
    CHECK(out_fb[py * CPS1_FB_WIDTH + px] == 0xFFFF,
                "mask clear: sprite should win over BG pixel, got %04x", out_fb[py * CPS1_FB_WIDTH + px]);

    masks.masks[2] = (uint16_t)(1u << 1); /* group 2, pen 1 -- matches the cell's actual pen */
    cps1_compositor_blend_priority(colors1, metas1, 1, sprite_fb, &masks, out_fb);
    CHECK(out_fb[py * CPS1_FB_WIDTH + px] == top_fb[py * CPS1_FB_WIDTH + px],
                "mask set: BG pixel should punch over the sprite, got %04x expected %04x",
                out_fb[py * CPS1_FB_WIDTH + px], top_fb[py * CPS1_FB_WIDTH + px]);

    free(gfx);

    if (failures) {
        fprintf(stderr, "[cps1-bg-selftest] FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-bg-selftest] OK\n");
    return 0;
}
