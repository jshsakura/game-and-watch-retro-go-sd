#include "cps1_rom.h"

int cps1_rom_attach(cps1_rom_t *rom, cps1_rom_region_t prg, cps1_rom_region_t gfx,
                     cps1_rom_region_t z80, cps1_rom_region_t oki)
{
    if (!prg.data || !prg.size || !gfx.data || !gfx.size)
        return -1;

    rom->prg = prg;
    rom->gfx = gfx;
    rom->z80 = z80;
    rom->oki = oki;
    return 0;
}

int cps1_rom_decode_tile(const cps1_rom_t *rom, uint32_t tile_index, uint8_t *out)
{
    uint32_t offset = tile_index * CPS1_TILE_SIZE_BYTES;
    if (offset + CPS1_TILE_SIZE_BYTES > rom->gfx.size)
        return -1;

    for (uint32_t i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        out[i] = rom->gfx.data[offset + i];
    return 0;
}

/*
 * Layout data below is transcribed verbatim from MAME's gfx_layout structs
 * (docs/CPS1_MAME_ALIGNMENT.md section 1 / src/mame/capcom/cps1.cpp:3837-
 * 3886), just with xoffset/yoffset arrays fully expanded (MAME's STEP8/16/32
 * macros generate these at compile time; this file writes them out since it
 * has no equivalent macro). Unused array tail entries are 0 and never read
 * (loops below are bounded by layout->width/height, not array capacity).
 */
const cps1_gfx_layout_t CPS1_GFX_LAYOUT_8X8_LEFT = {
    4, 8, 8,
    { 24, 16, 8, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 0, 64, 128, 192, 256, 320, 384, 448 },
    512,
};

const cps1_gfx_layout_t CPS1_GFX_LAYOUT_8X8_RIGHT = {
    4, 8, 8,
    { 24, 16, 8, 0, 0, 0, 0, 0 },
    { 32, 33, 34, 35, 36, 37, 38, 39 },
    { 0, 64, 128, 192, 256, 320, 384, 448 },
    512,
};

const cps1_gfx_layout_t CPS1_GFX_LAYOUT_16X16 = {
    4, 16, 16,
    { 24, 16, 8, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 32, 33, 34, 35, 36, 37, 38, 39 },
    { 0, 64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960 },
    1024,
};

const cps1_gfx_layout_t CPS1_GFX_LAYOUT_32X32 = {
    4, 32, 32,
    { 24, 16, 8, 0, 0, 0, 0, 0 },
    {
        0, 1, 2, 3, 4, 5, 6, 7,
        32, 33, 34, 35, 36, 37, 38, 39,
        64, 65, 66, 67, 68, 69, 70, 71,
        96, 97, 98, 99, 100, 101, 102, 103,
    },
    {
        0, 128, 256, 384, 512, 640, 768, 896,
        1024, 1152, 1280, 1408, 1536, 1664, 1792, 1920,
        2048, 2176, 2304, 2432, 2560, 2688, 2816, 2944,
        3072, 3200, 3328, 3456, 3584, 3712, 3840, 3968,
    },
    4096,
};

int cps1_rom_decode_tile_planar(const cps1_rom_t *rom, const cps1_gfx_layout_t *layout,
                                 uint32_t tile_index, uint8_t *out)
{
    if (layout->planes > 8 || layout->width > 32 || layout->height > 32)
        return -1;

    uint32_t base_bit = tile_index * layout->bits_per_tile;
    uint32_t base_byte = base_bit / 8;
    uint32_t tile_bytes = (layout->bits_per_tile + 7) / 8;
    if (base_byte + tile_bytes > rom->gfx.size)
        return -1;

    unsigned row_bytes = layout->width / 2;

    for (unsigned row = 0; row < layout->height; row++) {
        for (unsigned col = 0; col < layout->width; col++) {
            uint8_t pixel = 0;
            /* planebit starts at the MSB (planes-1) and shifts right per
             * plane, matching MAME's gfx_element::decode() exactly --
             * plane-array index 0 contributes the pixel's MSB. */
            for (unsigned p = 0; p < layout->planes; p++) {
                uint32_t bitno = base_bit + layout->planeoffset[p] + layout->yoffset[row] +
                                  layout->xoffset[col];
                uint32_t byte_idx = bitno / 8;
                unsigned bit_in_byte = bitno % 8;
                uint8_t bit = (uint8_t)((rom->gfx.data[byte_idx] >> (7 - bit_in_byte)) & 1u);
                if (bit)
                    pixel = (uint8_t)(pixel | (1u << (layout->planes - 1 - p)));
            }

            unsigned byte_idx = row * row_bytes + col / 2;
            if (col & 1)
                out[byte_idx] = (uint8_t)((out[byte_idx] & 0xF0u) | pixel);
            else
                out[byte_idx] = (uint8_t)((uint8_t)(pixel << 4) | (out[byte_idx] & 0x0Fu));
        }
    }
    return 0;
}
