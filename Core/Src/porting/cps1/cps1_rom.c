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
