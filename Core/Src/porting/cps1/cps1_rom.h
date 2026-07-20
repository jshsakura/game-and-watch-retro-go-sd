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
 * into `out`. Returns 0 on success, -1 if tile_index is out of range.
 *
 * TODO(cps1): this is a flat "tile_index * 32 bytes into one gfx blob"
 * decode -- real CPS-1 hardware spreads each tile's 4 bitplanes across
 * MULTIPLE separate ROM chips with a specific bit-interleave (see MAME's
 * cps1.cpp gfx_layout for the real wiring). Correct only for a
 * pre-flattened/pre-planar-merged GFX blob; a real ROM set needs a
 * per-game gfxdecode step before this function is accurate. */
int cps1_rom_decode_tile(const cps1_rom_t *rom, uint32_t tile_index, uint8_t *out);
