#include <stddef.h>

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
    rom->chips = NULL;
    return 0;
}

int cps1_rom_attach_chips(cps1_rom_t *rom, cps1_rom_region_t prg,
                           const cps1_gfx_chips_t *chips)
{
    if (!rom || !prg.data || !prg.size || !chips)
        return -1;
    if (chips->chip_count == 0 || chips->chip_count > CPS1_GFX_MAX_CHIPS ||
        chips->chip_size == 0)
        return -1;
    for (unsigned i = 0; i < chips->chip_count; i++)
        if (chips->chip[i] == NULL)
            return -1;

    rom->prg = prg;
    rom->gfx.data = NULL;
    rom->gfx.size = 0;
    rom->z80.data = NULL; rom->z80.size = 0;
    rom->oki.data = NULL; rom->oki.size = 0;
    rom->chips = chips;
    return 0;
}

uint32_t cps1_rom_gfx_size(const cps1_rom_t *rom)
{
    if (rom->chips != NULL)
        return rom->chips->chip_size * rom->chips->chip_count;
    return rom->gfx.size;
}

uint8_t cps1_rom_gfx_byte(const cps1_rom_t *rom, uint32_t off)
{
    if (rom->chips != NULL)
        return cps1_gfx_chip_byte(rom->chips, off);
    return (off < rom->gfx.size) ? rom->gfx.data[off] : 0;
}

int cps1_rom_decode_tile(const cps1_rom_t *rom, uint32_t tile_index, uint8_t *out)
{
    uint32_t offset = tile_index * CPS1_TILE_SIZE_BYTES;
    if (offset + CPS1_TILE_SIZE_BYTES > cps1_rom_gfx_size(rom))
        return -1;

    for (uint32_t i = 0; i < CPS1_TILE_SIZE_BYTES; i++)
        out[i] = cps1_rom_gfx_byte(rom, offset + i);
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

/*
 * wof/wofj's bank table (mapper_TK263B, docs/CPS1_MAME_ALIGNMENT.md
 * section 7): 2 banks of 0x8000 (32KB) each, contiguous across the shared
 * 64KB shifted-code address space -- see cps1_gfxrom_bank_mapper_wof's
 * doc comment in cps1_rom.h for why this makes the bank lookup an
 * identity for this specific game.
 */
typedef struct {
    uint32_t start, end;
    unsigned bank;
} cps1_gfx_range_t;

static const uint32_t s_wof_bank_sizes[2] = { 0x8000u, 0x8000u };
static const cps1_gfx_range_t s_wof_bank_table[] = {
    { 0x00000u, 0x07fffu, 0u },
    { 0x08000u, 0x0ffffu, 1u },
};

/* SPRITES/SCROLL1/SCROLL2/SCROLL3 -- cps1_v.cpp:2385-2420's
 * `shift = (type==SPRITES) ? 1 : (type==SCROLL1) ? 0 : (type==SCROLL2) ? 1 : 3`. */
static const unsigned char s_wof_shift_by_type[4] = { 1u, 0u, 1u, 3u };

uint32_t cps1_gfxrom_bank_mapper_wof(cps1_gfx_type_t type, uint32_t code)
{
    if ((unsigned)type > (unsigned)CPS1_GFXTYPE_SCROLL3)
        return 0;

    unsigned shift = s_wof_shift_by_type[type];
    uint32_t shifted = code << shift;

    for (unsigned i = 0; i < sizeof(s_wof_bank_table) / sizeof(s_wof_bank_table[0]); i++) {
        const cps1_gfx_range_t *r = &s_wof_bank_table[i];
        if (shifted >= r->start && shifted <= r->end) {
            uint32_t bank_base = 0;
            for (unsigned b = 0; b < r->bank; b++)
                bank_base += s_wof_bank_sizes[b];
            uint32_t mapped = bank_base + (shifted - r->start);
            return mapped >> shift;
        }
    }
    return 0; /* shifted code falls outside every bank -- caller must range-check */
}

int cps1_rom_check_reset_vector(const cps1_rom_region_t *prg, uint32_t *out_ssp, uint32_t *out_pc)
{
    if (!prg->data || prg->size < 8)
        return -1;

    uint32_t ssp = ((uint32_t)prg->data[0] << 24) | ((uint32_t)prg->data[1] << 16) |
                   ((uint32_t)prg->data[2] << 8) | (uint32_t)prg->data[3];
    uint32_t pc = ((uint32_t)prg->data[4] << 24) | ((uint32_t)prg->data[5] << 16) |
                  ((uint32_t)prg->data[6] << 8) | (uint32_t)prg->data[7];

    if (out_ssp) *out_ssp = ssp;
    if (out_pc)  *out_pc = pc;

    if (pc >= prg->size)
        return -1; /* PC doesn't point anywhere inside the loaded ROM */

    /*
     * `pc < size` ALONE DOES NOT CATCH A REVERSED BYTE-SWAP, which is the
     * single failure this check exists to catch. Worked example: a real
     * reset PC of 0x00000100, word-swapped, reads back as 0x00000001 --
     * still comfortably inside a 1 MB ROM, so the size test passes and the
     * ROM boots into garbage. (The packer's dry run with all-zero dummy
     * chips passing was the same hole: PC=0 < size.) Three cheap tests
     * close it, in increasing order of strength:
     */
    if (pc < 8u)
        return -1; /* PC pointing into the vector table's own SSP/PC longs */
    if (pc & 1u)
        return -1; /* odd PC -- a real 68000 takes an address error here */

    /*
     * The decisive one, and it is CPS-1-specific: the board's ONLY work RAM
     * is 0xFF0000-0xFFFFFF, so the initial supervisor stack pointer must
     * land in it (0x1000000 inclusive, because the stack pre-decrements
     * from the top). A correct SSP therefore looks like 0x00FF____; the
     * same value with its bytes swapped looks like 0xFF00____ and is
     * rejected on sight. This is what actually distinguishes "loaded
     * correctly" from "loaded backwards".
     */
    if (ssp < CPS1_WRAM_BASE_ADDR || ssp > CPS1_WRAM_TOP_ADDR)
        return -1;
    if (ssp & 1u)
        return -1; /* 68000 stack pointer is always word-aligned */

    return 0;
}

int cps1_rom_decode_tile_planar(const cps1_rom_t *rom, const cps1_gfx_layout_t *layout,
                                 uint32_t tile_index, uint8_t *out)
{
    if (layout->planes > 8 || layout->width > 32 || layout->height > 32)
        return -1;

    uint32_t base_bit = tile_index * layout->bits_per_tile;
    uint32_t base_byte = base_bit / 8;
    uint32_t tile_bytes = (layout->bits_per_tile + 7) / 8;
    if (base_byte + tile_bytes > cps1_rom_gfx_size(rom))
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
                uint8_t bit = (uint8_t)((cps1_rom_gfx_byte(rom, byte_idx) >> (7 - bit_in_byte)) & 1u);
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

/*
 * Decodes ONE 8x8 quadrant of a CPS-1 tile straight out of the raw GFX ROM,
 * into the packed 4bpp form the blitter wants (32 bytes, high nibble = even
 * pixel).
 *
 * This exists because the three scroll layers do NOT share a byte layout,
 * and treating them as if they did is what made a real ROM render as a
 * correct SCROLL1 over a scrambled SCROLL2/SCROLL3. From MAME's own
 * gfx_layouts (cps1.cpp:3837+), all three put the four bitplanes of an
 * 8-pixel span in FOUR CONSECUTIVE BYTES -- plane order LSB..MSB, MSB of
 * each byte being the leftmost pixel -- but they differ in how rows and
 * 8-pixel spans are spaced:
 *
 *   8x8    (cps1_layout8x8)    64 B/tile,  8 rows,  8 B/row, span at +0
 *                              (its RIGHT-half partner lives at +4)
 *   16x16  (cps1_layout16x16) 128 B/tile, 16 rows,  8 B/row, spans at +0,+4
 *   32x32  (cps1_layout32x32) 512 B/tile, 32 rows, 16 B/row, spans at +0..+12
 *
 * So a quadrant (qx,qy) of a `sub`x`sub` block starts at
 *     code * (sub*sub*32) + qy * (8 * row_stride) + qx * 4
 * with row_stride 8 for sub 1 and 2, 16 for sub 4.
 */
int cps1_rom_decode_subtile(const cps1_rom_t *rom, unsigned sub, uint32_t code,
                             unsigned qx, unsigned qy, uint8_t *out)
{
    if (!rom || (!rom->gfx.data && !rom->chips) || !out || sub == 0 || sub > 4)
        return -1;
    if (qx >= sub || qy >= sub)
        return -1;

    const uint32_t row_stride = (sub == 4u) ? 16u : 8u;
    const uint32_t tile_bytes = sub * sub * 32u;
    const uint32_t base = code * tile_bytes + qy * (8u * row_stride) + qx * 4u;

    if (base + 7u * row_stride + 4u > cps1_rom_gfx_size(rom))
        return -1;

    for (unsigned y = 0; y < 8; y++) {
        const uint32_t row = base + y * row_stride;
        /* p[0]=plane0(LSB) p[1]=plane1 p[2]=plane2 p[3]=plane3(MSB). Lifted
         * out of the pixel loop: with chip-gathered graphics each byte costs
         * an interleave calculation, and the inner loop would otherwise pay
         * it eight times over for the same four bytes. */
        const uint8_t p[4] = {
            cps1_rom_gfx_byte(rom, row + 0u), cps1_rom_gfx_byte(rom, row + 1u),
            cps1_rom_gfx_byte(rom, row + 2u), cps1_rom_gfx_byte(rom, row + 3u),
        };
        for (unsigned x = 0; x < 8; x += 2) {
            unsigned pix_hi = 0, pix_lo = 0;
            unsigned sh_hi = 7u - x, sh_lo = 7u - (x + 1u);
            for (unsigned pl = 0; pl < 4; pl++) {
                pix_hi |= ((p[pl] >> sh_hi) & 1u) << pl;
                pix_lo |= ((p[pl] >> sh_lo) & 1u) << pl;
            }
            out[y * 4u + x / 2u] = (uint8_t)((pix_hi << 4) | pix_lo);
        }
    }
    return 0;
}

uint8_t cps1_gfx_chip_byte(const cps1_gfx_chips_t *g, uint32_t off)
{
    if (!g || g->chip_count == 0 || g->chip_size == 0)
        return 0;

    /* Two 4-chip halves: 0x000000.. and 0x200000.. (see the header). A set
     * with only 4 chips has no upper half and simply range-fails there. */
    const uint32_t half_bytes = g->chip_size * 4u;
    uint32_t half = off / half_bytes;
    uint32_t o = off - half * half_bytes;

    unsigned chip = (unsigned)(half * 4u + (o % 8u) / 2u);
    uint32_t index = (o / 8u) * 2u + (o % 2u);

    if (chip >= g->chip_count || index >= g->chip_size || !g->chip[chip])
        return 0;
    return g->chip[chip][index];
}
