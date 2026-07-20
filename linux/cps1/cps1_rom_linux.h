#pragma once
/* Host-only ROM chip loader (fopen/fread) -- not freestanding, so it lives
 * in linux/cps1/, not Core/Src/porting/cps1/. Fills a cps1_rom_t with
 * malloc'd buffers via cps1_rom_attach(). z80_path/oki_path may be NULL. */
#include "cps1_rom.h"

int cps1_rom_load_linux(cps1_rom_t *rom, const char *prg_path, const char *gfx_path,
                         const char *z80_path, const char *oki_path);
