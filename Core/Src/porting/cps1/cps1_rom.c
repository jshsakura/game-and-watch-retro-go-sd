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

const cps1_gfx_layout_t CPS1_GFX_LAYOUT_DEFAULT = {
    4,                    /* planes */
    {0, 8, 16, 24, 0, 0, 0, 0}, /* plane_byte_offset */
    1,                    /* bytes_per_row_per_plane */
    CPS1_TILE_SIZE_BYTES, /* tile_stride_bytes */
};

int cps1_rom_decode_tile_planar(const cps1_rom_t *rom, const cps1_gfx_layout_t *layout,
                                 uint32_t tile_index, uint8_t *out)
{
    uint32_t base = tile_index * layout->tile_stride_bytes;
    if (base + layout->tile_stride_bytes > rom->gfx.size)
        return -1;
    if (layout->planes > 8)
        return -1;

    for (int row = 0; row < 8; row++) {
        uint8_t plane_byte[8];
        for (unsigned p = 0; p < layout->planes; p++)
            plane_byte[p] = rom->gfx.data[base + layout->plane_byte_offset[p] +
                                           (uint32_t)row * layout->bytes_per_row_per_plane];

        for (int col = 0; col < 8; col++) {
            int bit_pos = 7 - col; /* MSB = leftmost pixel */
            uint8_t pixel = 0;
            for (unsigned p = 0; p < layout->planes; p++)
                pixel = (uint8_t)(pixel | (((plane_byte[p] >> bit_pos) & 1u) << p));

            int byte_idx = row * 4 + col / 2;
            if (col & 1)
                out[byte_idx] = (uint8_t)((out[byte_idx] & 0xF0u) | pixel);
            else
                out[byte_idx] = (uint8_t)((uint8_t)(pixel << 4) | (out[byte_idx] & 0x0Fu));
        }
    }
    return 0;
}
