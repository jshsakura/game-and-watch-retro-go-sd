#ifndef STUB_RG_CLOCK_STATE_H
#define STUB_RG_CLOCK_STATE_H
#include <stddef.h>
#include <stdint.h>
/* Host-test stub: rg_clock_album.c only needs this one accessor (the decode
 * arena past .overlay_clock's own footprint). */
uint8_t *clock_overlay_arena(size_t *out_bytes);
#endif
