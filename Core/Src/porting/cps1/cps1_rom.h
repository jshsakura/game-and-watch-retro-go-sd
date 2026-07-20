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

#define CPS1_TILE_SIZE_BYTES 32 /* 8x8 pixels, 4bpp packed (1 nibble/pixel) */

/* Copies one packed 4bpp 8x8 tile (32 bytes) from rom->gfx at `tile_index`
 * into `out`. Returns 0 on success, -1 if tile_index is out of range. Flat
 * "tile_index * 32 bytes into one gfx blob" -- correct only if rom->gfx is
 * ALREADY packed 4bpp (true for the synthetic test data in cps1_core.c;
 * NOT true for a real CPS-1 GFX ROM dump -- use cps1_rom_decode_tile_planar
 * for that). */
int cps1_rom_decode_tile(const cps1_rom_t *rom, uint32_t tile_index, uint8_t *out);

/*
 * Real CPS-1 GFX ROMs store each tile as separate 1-bit-per-pixel
 * bitplanes, not pre-packed 4bpp nibbles -- this is the generalization
 * that decodes those, parameterized like MAME's gfx_layout (planes +
 * per-plane offset) so a confirmed real layout is a constant swap, not a
 * rewrite. Simplified vs. MAME's raw bit-level gfx_layout: offsets here
 * are BYTE-granular (one plane byte per row, MSB = leftmost pixel), which
 * covers the common byte-aligned-bitplane convention but not arbitrary
 * bit-level interleaves.
 *
 * UNCONFIRMED for CPS-1 specifically: CPS1_GFX_LAYOUT_DEFAULT is the
 * "4 contiguous 8-byte bitplanes per 8x8 tile" convention common to many
 * Capcom-era boards -- NOT verified against MAME's cps1.cpp (this session
 * could not retrieve that source; see docs/CPS1_FEASIBILITY.md section 6).
 * Treat its output as unverified until cross-checked against a real ROM
 * dump or MAME's actual gfx_layout for cps1.
 */
typedef struct {
    uint8_t planes;                    /* bits per pixel, e.g. 4 */
    uint16_t plane_byte_offset[8];     /* byte offset of plane p's row 0, within the tile block */
    uint16_t bytes_per_row_per_plane;  /* usually 1 (8 pixels/row fits one byte) */
    uint16_t tile_stride_bytes;        /* total bytes per tile in the gfx region */
} cps1_gfx_layout_t;

extern const cps1_gfx_layout_t CPS1_GFX_LAYOUT_DEFAULT;

int cps1_rom_decode_tile_planar(const cps1_rom_t *rom, const cps1_gfx_layout_t *layout,
                                 uint32_t tile_index, uint8_t *out);
