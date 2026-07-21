#pragma once
/*
 * CPS-1 ROM regions -- freestanding: holds pointers/sizes the caller
 * already mapped. Host (linux/cps1/cps1_rom_linux.c) mallocs and fread()s
 * each chip dump; the device version (OSPI XIP, docs/CPS1_FEASIBILITY.md
 * strategy 1) does not exist yet -- it would just point `prg`/`gfx` at the
 * memory-mapped flash address instead of a malloc'd buffer, with zero
 * other code changes, which is the point of keeping this struct pointer-
 * based instead of owning storage.
 *
 * CPS-1 games ship as several separate ROM chip dumps (68000 program, CHR/
 * tile graphics across multiple GFX chips, Z80 sound program, OKI6295
 * ADPCM samples) -- this skeleton treats each as one flat region and does
 * NOT yet handle multi-chip GFX interleaving (see cps1_rom_decode_tile).
 */
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint32_t size;
} cps1_rom_region_t;

typedef struct {
    cps1_rom_region_t prg; /* 68000 program ROM */
    cps1_rom_region_t gfx; /* CHR/tile graphics ROM(s), pre-swizzle */
    cps1_rom_region_t z80; /* Z80 sound program ROM */
    cps1_rom_region_t oki; /* OKI6295 ADPCM sample ROM */
} cps1_rom_t;

/* Validates and stores the regions the caller already mapped. prg and gfx
 * are required (z80/oki may be {0,0} while sound is unimplemented).
 * Returns 0 on success, -1 if a required region is missing/empty. */
int cps1_rom_attach(cps1_rom_t *rom, cps1_rom_region_t prg, cps1_rom_region_t gfx,
                     cps1_rom_region_t z80, cps1_rom_region_t oki);

/*
 * Packed 4bpp OUTPUT size for an 8x8 tile: 2 pixels/byte * 64 pixels = 32
 * bytes. This is the tile CACHE's storage format (cps1_ppu.h) and what the
 * synthetic pre-packed test data in cps1_core.c and the selftest programs
 * already is --
 * it does NOT change with the raw ROM bitplane layout below. Do not
 * confuse this with a layout's RAW ROM byte consumption per tile (a real
 * CPS-1 8x8 half-tile consumes 64 raw bytes -- see cps1_gfx_layout_t's own
 * bits_per_tile field, confirmed in docs/CPS1_MAME_ALIGNMENT.md section 1).
 * A single global "tile size" constant cannot represent both numbers at
 * once, and conflating them was an imprecision in that doc's section 8
 * table -- corrected here during implementation.
 */
#define CPS1_TILE_SIZE_BYTES 32

/* Copies one packed 4bpp 8x8 tile (32 bytes) from rom->gfx at `tile_index`
 * into `out`. Returns 0 on success, -1 if tile_index is out of range. Flat
 * "tile_index * 32 bytes into one gfx blob" -- correct only if rom->gfx is
 * ALREADY packed 4bpp (true for the synthetic test data in cps1_core.c;
 * NOT true for a real CPS-1 GFX ROM dump -- use cps1_rom_decode_tile_planar
 * for that). */
int cps1_rom_decode_tile(const cps1_rom_t *rom, uint32_t tile_index, uint8_t *out);

/*
 * Real CPS-1 GFX ROM bitplane layout -- CONFIRMED against MAME source
 * (docs/CPS1_MAME_ALIGNMENT.md section 1, `src/mame/capcom/cps1.cpp`'s
 * `GFXDECODE_START(gfx_cps1)` and `src/emu/drawgfx.cpp`'s actual
 * `gfx_element::decode()`). Mirrors MAME's `gfx_layout` shape directly (bit
 * offsets, not byte offsets) rather than a simplified byte-granular
 * approximation, because the real layout genuinely interleaves all 4
 * planes WITHIN each row (not as contiguous per-plane blocks) and needs
 * bit-precise addressing to decode correctly.
 *
 * IMPORTANT: `planeoffset[0]` contributes the pixel's MSB, `planeoffset
 * [planes-1]` the LSB -- this matches MAME's `gfx_element::decode()`
 * (`planebit` starts at `1 << (planes-1)` and shifts right each plane),
 * which is the OPPOSITE of "plane 0 = LSB" a first guess might assume.
 */
typedef struct {
    uint8_t planes;              /* bits per pixel, e.g. 4 */
    uint8_t width, height;       /* tile dimensions in pixels (height <= 32) */
    uint16_t planeoffset[8];     /* BIT offset per plane; index 0 = pixel MSB */
    uint16_t xoffset[32];        /* BIT offset per column */
    uint16_t yoffset[32];        /* BIT offset per row */
    uint32_t bits_per_tile;      /* total bits consumed per tile_index (raw ROM stride) */
} cps1_gfx_layout_t;

/* cps1_layout8x8 (cps1.cpp:3837): SCROLL1's left 8x8 half of a 16x16
 * physical block. 64 raw bytes/tile. */
extern const cps1_gfx_layout_t CPS1_GFX_LAYOUT_8X8_LEFT;
/* cps1_layout8x8_2 (cps1.cpp:3848): the same block's right half
 * (xoffset starts at bit 32 = byte 4, not bit 0). Selected by
 * BIT(tilemap_column_index, 5), not by tile code -- see the doc. */
extern const cps1_gfx_layout_t CPS1_GFX_LAYOUT_8X8_RIGHT;
/* cps1_layout16x16 (cps1.cpp:3859): SCROLL2 / OBJ sprites. Whole 16x16
 * block, both halves. 128 raw bytes/tile. Not yet wired into the sub-tile
 * cache (cps1_ppu.c/cps1_bg.c always fetch 8x8 sub-tiles) -- provided here
 * as confirmed reference data for when that changes. */
extern const cps1_gfx_layout_t CPS1_GFX_LAYOUT_16X16;
/* cps1_layout32x32 (cps1.cpp:3870): SCROLL3, four vertically-adjacent
 * 8-wide x 32-tall strips (NOT a 4x4 grid of 8x8 sub-tiles). 512 raw
 * bytes/tile. Same "not yet wired into the sub-tile cache" note applies. */
extern const cps1_gfx_layout_t CPS1_GFX_LAYOUT_32X32;

/* SCROLL1's left half-tile is the default single-8x8-tile decode target. */
#define CPS1_GFX_LAYOUT_DEFAULT CPS1_GFX_LAYOUT_8X8_LEFT

/* Decodes one tile per `layout` from rom->gfx at `tile_index` into `out`,
 * packed 4bpp (2 pixels/byte, row-major, MSB-first nibble -- same output
 * convention as cps1_rom_decode_tile). `out` must be
 * layout->width*layout->height/2 bytes (32 for the 8x8 layouts -- exactly
 * CPS1_TILE_SIZE_BYTES). Returns 0 on success, -1 if tile_index is out of
 * range for rom->gfx. */
int cps1_rom_decode_tile_planar(const cps1_rom_t *rom, const cps1_gfx_layout_t *layout,
                                 uint32_t tile_index, uint8_t *out);
