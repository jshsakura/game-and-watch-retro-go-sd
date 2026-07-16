/* Typed CD/MD globals the core references but never accesses for a 32X cart.
 * Defined with their real types (so &sym has the right shape); zero-initialised. */
#include "pico/pico_int.h"
#include "pico/memory.h"

M68K_CONTEXT PicoCpuFS68k;
mcd_state *Pico_mcd;
uptr s68k_read8_map  [0x1000000 >> M68K_MEM_SHIFT];
uptr s68k_read16_map [0x1000000 >> M68K_MEM_SHIFT];
char Pico_msd[4096];
