#pragma once
#include <stdlib.h>
/* The device puts the PPU's VRAM in ITC RAM. On the host, plain calloc: the test
 * is about what ppu_saveload restores, not about where VRAM lives. */
static inline void *itc_calloc(size_t n, size_t sz) { return calloc(n, sz); }
