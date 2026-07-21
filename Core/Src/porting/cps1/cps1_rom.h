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
 * Raw MAME GFX chips, read WITHOUT assembling them.
 *
 * MAME stores CPS-1 graphics as 8 chips interleaved 2 bytes at a time across
 * an 8-byte stride (ROM_LOAD64_WORD), and the obvious reading is that a port
 * must first assemble them into one 4 MB image. That is what this project's
 * .cps1 container was invented to do -- and it is unnecessary. The interleave
 * is pure address arithmetic:
 *
 *     half  = (offset >= 0x200000)          // the romset's two 4-chip halves
 *     o     = offset - half * 0x200000
 *     chip  = half * 4 + (o % 8) / 2
 *     index = (o / 8) * 2 + (o % 2)
 *
 * VERIFIED, not assumed: 200,000 random offsets over wofj's real chips
 * reproduce the assembled 4 MB image byte for byte, zero mismatches.
 *
 * This matters because 4 MB does not fit RAM_EMU (724 KB), so an assembled
 * image would have to be built into external flash and cached -- a bespoke
 * container, a converter, and a user-facing conversion step, all to avoid an
 * address calculation. Reading the chips in place costs one extra
 * discontiguous read per 4-byte plane group, which on XIP flash is just
 * another load. Drop the extracted MAME romset in a folder and it works.
 */
#define CPS1_GFX_MAX_CHIPS 8

typedef struct {
    const uint8_t *chip[CPS1_GFX_MAX_CHIPS];
    uint32_t chip_size;      /* every CPS-1 GFX chip in a set is the same size */
    unsigned chip_count;
} cps1_gfx_chips_t;

/* Byte at interleaved offset `off`, gathered from whichever chip holds it.
 * Returns 0 if the offset falls outside the set. */
uint8_t cps1_gfx_chip_byte(const cps1_gfx_chips_t *g, uint32_t off);

/* Decodes one 8x8 quadrant (qx,qy) of a sub x sub block (sub = 1, 2 or 4 for
 * SCROLL1/SCROLL2/SCROLL3) from the raw GFX ROM into 32 packed 4bpp bytes.
 * See the implementation comment for why each layer needs its own row
 * stride and span offset. Returns 0, or -1 if the tile falls outside the
 * ROM. */
int cps1_rom_decode_subtile(const cps1_rom_t *rom, unsigned sub, uint32_t code,
                             unsigned qx, unsigned qy, uint8_t *out);

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

/*
 * GFX ROM bank mapping -- CONFIRMED for `wof`/`wofj` specifically
 * (docs/CPS1_MAME_ALIGNMENT.md section 7, cps1_v.cpp:2385-2420 +
 * mapper_TK263B_table, cps1_v.cpp:1373-1384). Real CPS-1 hardware shares
 * ONE 64KB "tile-code" address space across all four gfx types, but each
 * type's OWN native code granularity differs (SCROLL1 is the finest, 8x8;
 * SPRITES/SCROLL2 are 16x16; SCROLL3 is 32x32) -- gfxrom_bank_mapper's
 * type-dependent left-shift normalizes a type's native code into that
 * shared address space before a per-game bank table decides which
 * physical ROM bank (after byte-interleaving, see cps1_rom_load_
 * interleaved) it actually lives in.
 */
typedef enum {
    CPS1_GFXTYPE_SPRITES = 0,
    CPS1_GFXTYPE_SCROLL1 = 1,
    CPS1_GFXTYPE_SCROLL2 = 2,
    CPS1_GFXTYPE_SCROLL3 = 3,
} cps1_gfx_type_t;

/*
 * wof/wofj's own bank mapper (`CPS_B_21_QS1`/`mapper_TK263B`): converts a
 * raw per-type tile/sprite code into the tile_index cps1_rom_decode_tile_
 * planar expects when paired with that type's OWN cps1_gfx_layout_t
 * (CPS1_GFX_LAYOUT_16X16 for SPRITES/SCROLL2, _32X32 for SCROLL3,
 * _8X8_LEFT for SCROLL1), against ONE flat, byte-interleaved GFX ROM blob.
 *
 * General mechanism (matches MAME's gfxrom_bank_mapper exactly): shift the
 * code by the type's shift, find which of the game's gfx_range bank
 * entries contains the shifted value, mask it into that bank's position
 * (bank_base + (shifted - range.start), bank_base = sum of every earlier
 * bank's size), then shift back down by the SAME amount. wof's own table
 * (mapper_TK263B) happens to define exactly 2 CONTIGUOUS 32KB banks
 * spanning the shifted value's entire 0-0xFFFF range with no gaps -- so
 * for this specific game the bank lookup/recompose step is mathematically
 * an identity (CONFIRMED: docs/CPS1_MAME_ALIGNMENT.md section 7's own
 * "good news" callout -- "the bank split is fully transparent"). This
 * function still implements the general shift+bank-lookup steps (not a
 * hardcoded `code << shift` shortcut) so it generalizes to a future
 * non-contiguous-bank title's table without a rewrite -- only wof's own
 * bank table is wired in here. Returns 0 (a harmless but wrong index --
 * caller must range-check the result against rom->gfx.size before using
 * it) if `type` is invalid or the shifted code falls outside every bank.
 *
 * NOTE: this function does not yet feed the live renderer -- cps1_ppu.c/
 * cps1_bg.c still decompose 16x16/32x32 tiles into 4/16 consecutive 8x8
 * sub-tile fetches (a documented Phase 7-10 simplification, NOT the real
 * ROM's actual ONE-contiguous-block-per-tile layout the big gfx_layout
 * structs describe) -- rewiring the renderer to call this mapper + the
 * big layouts directly is real ROM art content it can't decode. Wiring
 * that up is future work once real ROM data exists to render, not part
 * of this phase's ask (implement the mapper + prove it against MAME's
 * confirmed formula, not migrate the renderer).
 */
uint32_t cps1_gfxrom_bank_mapper_wof(cps1_gfx_type_t type, uint32_t code);

/*
 * 68000 reset vector sanity check (docs/CPS1_MAME_ALIGNMENT.md section 9,
 * Phase 11 plan item 3): CPS-1 program ROM is memory-mapped starting at
 * 0x000000 (standard 68000 convention -- bytes 0-3 = initial SSP, bytes
 * 4-7 = initial PC, both big-endian). A validly-loaded PRG ROM's own PC
 * must therefore point somewhere WITHIN the loaded ROM's own byte range --
 * the cheapest possible correctness gate before trying to run any 68000
 * code from a real ROM dump, and one that reliably catches a wrong byte-
 * interleave direction, a truncated/corrupt dump, or simply the wrong
 * file (all typically produce a wildly out-of-range PC) long before any
 * instruction actually runs. Returns 0 and fills out_ssp/out_pc (either
 * may be NULL) if the vector's PC is in-range, -1 otherwise (including
 * prg->size < 8, too small to even hold a reset vector).
 */
/* CPS-1 work RAM occupies 0xFF0000-0xFFFFFF and is the board's only RAM, so
 * a valid initial supervisor stack pointer must live there (top inclusive --
 * the 68000 stack pre-decrements). cps1_rom_check_reset_vector() uses this to
 * tell a correctly-loaded ROM (SSP 0x00FF____) from a byte-swapped one
 * (0xFF00____), which a plain in-range test on PC cannot do. */
#define CPS1_WRAM_BASE_ADDR 0x00FF0000u
#define CPS1_WRAM_TOP_ADDR  0x01000000u

int cps1_rom_check_reset_vector(const cps1_rom_region_t *prg, uint32_t *out_ssp, uint32_t *out_pc);
