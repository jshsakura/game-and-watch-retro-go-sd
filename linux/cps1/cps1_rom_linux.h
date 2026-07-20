#pragma once
/* Host-only ROM chip loader (fopen/fread) -- not freestanding, so it lives
 * in linux/cps1/, not Core/Src/porting/cps1/. Fills a cps1_rom_t with
 * malloc'd buffers via cps1_rom_attach(). z80_path/oki_path may be NULL. */
#include "cps1_rom.h"

int cps1_rom_load_linux(cps1_rom_t *rom, const char *prg_path, const char *gfx_path,
                         const char *z80_path, const char *oki_path);

/* CPS-1 GFX (and often PRG) ROMs ship as multiple same-size chip dumps that
 * must be byte-interleaved into one region: chip0=1234, chip1=ABCD ->
 * combined=1A2B3C4D (confirmed convention, arcade-projects.com CPS1
 * conversion threads). Reads chip_count files (all must be the same size)
 * and interleaves them into a freshly malloc'd region. Returns 0 on
 * success, -1 on any read error or size mismatch. */
int cps1_rom_load_interleaved(cps1_rom_region_t *out, const char *const *paths, int chip_count);
