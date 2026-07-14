#ifndef GBA_IDLE_LOOP_H
#define GBA_IDLE_LOOP_H

#include <stdint.h>

/* The busy-wait PC for a cart, or 0 if we do not have one for it.
 *
 * gamepak_code is the 4 bytes at rom[0xAC] — not NUL-terminated, exactly 4 read.
 * Applied by main_gba.c AFTER load_gamepak(), so it overrides whatever gpSP's own
 * gba_over.h decided; see gba_idle_loop.c for why that is the right way round.
 */
uint32_t gba_idle_loop_lookup(const char *gamepak_code);

#endif /* GBA_IDLE_LOOP_H */
