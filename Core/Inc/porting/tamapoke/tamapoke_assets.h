/* Single-file asset container, /roms/homebrew/tamapoke_assets.dat.
 * See tamapoke_assets.cpp for why the packs are not loose files on the card.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Reads the index. Cheap, and safe to call more than once. */
bool tamapoke_assets_open(void);

/* Copies one entry into dst. Returns its size, or 0 if it is absent or larger
 * than cap -- the buffers are statically sized to the rescaled assets, so an
 * oversized entry means the wrong pipeline produced the .dat. */
uint32_t tamapoke_assets_read(const char *name, uint8_t *dst, uint32_t cap);

bool tamapoke_assets_has(const char *name);
