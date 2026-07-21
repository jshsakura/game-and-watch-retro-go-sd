#include "cps1_bg.h"

void cps1_bg_reset(cps1_bg_state_t *bg)
{
    for (int l = 0; l < CPS1_BG_LAYER_COUNT; l++) {
        cps1_bg_layer_t *layer = &bg->layers[l];
        layer->scroll_x = 0;
        layer->scroll_y = 0;
        for (int i = 0; i < CPS1_BG_MAP_W * CPS1_BG_MAP_H; i++) {
            layer->cells[i].code = 0;
            layer->cells[i].attr = 0;
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

uint32_t cps1_bg_swizzle_col_row_to_offset(unsigned layer_index, unsigned col, unsigned row)
{
    switch (layer_index) {
    case CPS1_BG_SCROLL1:
        return (row & 0x1Fu) | ((col & 0x3Fu) << 5) | ((row & 0x20u) << 6);
    case CPS1_BG_SCROLL2:
        return (row & 0x0Fu) | ((col & 0x3Fu) << 4) | ((row & 0x30u) << 6);
    default: /* CPS1_BG_SCROLL3 */
        return (row & 0x07u) | ((col & 0x3Fu) << 3) | ((row & 0x38u) << 6);
    }
}

void cps1_bg_swizzle_offset_to_col_row(unsigned layer_index, uint32_t offset,
                                        unsigned *out_col, unsigned *out_row)
{
    switch (layer_index) {
    case CPS1_BG_SCROLL1:
        *out_row = (offset & 0x1Fu) | ((offset >> 6) & 0x20u);
        *out_col = (offset >> 5) & 0x3Fu;
        break;
    case CPS1_BG_SCROLL2:
        *out_row = (offset & 0x0Fu) | ((offset >> 6) & 0x30u);
        *out_col = (offset >> 4) & 0x3Fu;
        break;
    default: /* CPS1_BG_SCROLL3 */
        *out_row = (offset & 0x07u) | ((offset >> 6) & 0x38u);
        *out_col = (offset >> 3) & 0x3Fu;
        break;
    }
}

void cps1_bg_render_layer(const cps1_bg_layer_t *layer, unsigned layer_index,
                           const cps1_rom_t *rom, cps1_tile_cache_t *cache,
                           const cps1_palette_t *pal, uint16_t *out_fb, uint8_t *out_meta)
{
    unsigned tile_px = cps1_bg_tile_px(layer_index);
    unsigned sub = tile_px / 8u; /* sub-tiles per side: 1, 2, or 4 */
    unsigned palette_offset = cps1_bg_layer_palette_offset(layer_index);

    for (int cy = 0; cy < CPS1_BG_MAP_H; cy++) {
        int cell_y0 = cy * (int)tile_px - layer->scroll_y;
        if (cell_y0 + (int)tile_px <= 0 || cell_y0 >= CPS1_FB_HEIGHT)
            continue;

        for (int cx = 0; cx < CPS1_BG_MAP_W; cx++) {
            int cell_x0 = cx * (int)tile_px - layer->scroll_x;
            if (cell_x0 + (int)tile_px <= 0 || cell_x0 >= CPS1_FB_WIDTH)
                continue;

            const cps1_bg_cell_t *cell = &layer->cells[cy * CPS1_BG_MAP_W + cx];
            unsigned bank = palette_offset + cps1_bg_attr_color(cell->attr);
            int flip_x = (int)cps1_bg_attr_flip_x(cell->attr);
            int flip_y = (int)cps1_bg_attr_flip_y(cell->attr);
            uint8_t prio = (uint8_t)cps1_bg_attr_priority(cell->attr);

            cps1_blit_block_indexed((uint32_t)cell->code, sub, bank, pal,
                                     cell_x0, cell_y0, flip_x, flip_y, cache, rom,
                                     out_fb, out_meta, prio);
        }
    }
}

void cps1_compositor_blend_priority(const uint16_t *const *colors,
                                     const uint8_t *const *metas,
                                     unsigned layer_count,
                                     const uint16_t *sprite_fb,
                                     const cps1_priority_masks_t *masks,
                                     uint16_t *out)
{
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        uint16_t bg_color = 0;
        uint8_t bg_meta = 0;
        for (unsigned l = 0; l < layer_count; l++) {
            if (metas[l][i] & CPS1_PIXEL_META_VALID) {
                bg_color = colors[l][i];
                bg_meta = metas[l][i];
            }
        }

        uint16_t final_color = bg_color;
        uint16_t spr = sprite_fb[i];
        if (spr != 0) {
            int bg_wins = 0;
            if (bg_meta & CPS1_PIXEL_META_VALID) {
                unsigned group = (bg_meta >> CPS1_PIXEL_META_GROUP_SHIFT) & CPS1_PIXEL_META_GROUP_MASK;
                unsigned pen = bg_meta & CPS1_PIXEL_META_PEN_MASK;
                if (masks->masks[group] & (1u << pen))
                    bg_wins = 1;
            }
            if (!bg_wins)
                final_color = spr;
        }
        out[i] = final_color;
    }
}
