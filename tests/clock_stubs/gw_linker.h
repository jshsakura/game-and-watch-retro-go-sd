/* Host-test stub for the linker-placed overlay symbols the MP3-alarm module
 * stages into. On the device these carry the overlay's RAM_EMU address and its
 * size (SIZEOF-as-address linker trick); the module test provides real backing
 * buffers for the pointer symbols and --defsym small values for the size ones. */
#pragma once
#include <stdint.h>

extern void   *__RAM_EMU_START__[];
extern void   *__RAM_EMU_END__[];
extern void   *_OVERLAY_MUSIC_BSS_START[];
extern uint8_t  _OVERLAY_MUSIC_BSS_SIZE;
extern uint8_t  _OVERLAY_MUSIC_SIZE;
extern void   *_OVERLAY_CLOCK_BSS_START[];
extern void   *_OVERLAY_CLOCK_BSS_END[];
extern uint8_t  _OVERLAY_CLOCK_BSS_SIZE;
extern uint8_t  _OVERLAY_CLOCK_SIZE;
