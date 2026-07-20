#include "cps1_bg.h"

void cps1_bg_reset(cps1_bg_state_t *bg)
{
    for (int l = 0; l < CPS1_BG_LAYER_COUNT; l++) {
        cps1_bg_layer_t *layer = &bg->layers[l];
        layer->scroll_x = 0;
        layer->scroll_y = 0;
        for (int i = 0; i < CPS1_BG_MAP_W * CPS1_BG_MAP_H; i++) {
            layer->cells[i].tile_index = 0;
            layer->cells[i].palette = 0;
            layer->cells[i].enabled = 0;
        }
    }
}

unsigned cps1_bg_tile_px(unsigned layer_index)
{
    switch (layer_index) {
    case CPS1_BG_SCROLL1: return 8;
    case CPS1_BG_SCROLL2: return 16;
    default:              return 32; /* CPS1_BG_SCROLL3 */
    }
}

void cps1_bg_render_layer(const cps1_bg_layer_t *layer, unsigned layer_index,
                           const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                           const cps1_palette_t *pal, uint16_t *out_fb)
{
    unsigned tile_px = cps1_bg_tile_px(layer_index);
    unsigned sub = tile_px / 8u; /* sub-tiles per side: 1, 2, or 4 */

    for (int cy = 0; cy < CPS1_BG_MAP_H; cy++) {
        int cell_y0 = cy * (int)tile_px - layer->scroll_y;
        if (cell_y0 + (int)tile_px <= 0 || cell_y0 >= CPS1_FB_HEIGHT)
            continue;

        for (int cx = 0; cx < CPS1_BG_MAP_W; cx++) {
            int cell_x0 = cx * (int)tile_px - layer->scroll_x;
            if (cell_x0 + (int)tile_px <= 0 || cell_x0 >= CPS1_FB_WIDTH)
                continue;

            const cps1_bg_cell_t *cell = &layer->cells[cy * CPS1_BG_MAP_W + cx];
            if (!cell->enabled)
                continue;

            for (unsigned sy = 0; sy < sub; sy++) {
                for (unsigned sx = 0; sx < sub; sx++) {
                    uint32_t sub_tile = (uint32_t)cell->tile_index + sy * sub + sx;
                    const uint8_t *tile = cps1_tile_cache_fetch(cache, rom, sub_tile);
                    if (!tile)
                        continue;
                    int dst_x = cell_x0 + (int)(sx * 8u);
                    int dst_y = cell_y0 + (int)(sy * 8u);
                    cps1_blit8x8_indexed(tile, cell->palette, pal, dst_x, dst_y, out_fb);
                }
            }
        }
    }
}

void cps1_compositor_blend(const uint16_t *bottom, const uint16_t *middle,
                            const uint16_t *top, uint16_t *out)
{
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        uint16_t px = bottom[i];
        if (middle[i] != 0)
            px = middle[i];
        if (top[i] != 0)
            px = top[i];
        out[i] = px;
    }
}
