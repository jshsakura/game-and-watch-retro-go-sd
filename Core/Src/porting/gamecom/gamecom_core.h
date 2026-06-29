// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Robbbert
//
// Freestanding C port of MAME's Tiger Game.com machine (gamecom_m.cpp /
// gamecom_v.cpp / gamecom.cpp).  No MAME framework: plain bus, banking, DMA,
// timers and a software scanline renderer into an 8bpp palette framebuffer.
#ifndef GAMECOM_CORE_H
#define GAMECOM_CORE_H

#include <stdint.h>

#define GAMECOM_W 200
#define GAMECOM_H 160

/* Button bitmasks: ACTIVE-LOW (a 0 bit = pressed), matching the SM8521 P-ports.
   in0 (keys row 0x7FFF): */
#define GC_IN0_UP     0x01
#define GC_IN0_DOWN   0x02
#define GC_IN0_LEFT   0x04
#define GC_IN0_RIGHT  0x08
#define GC_IN0_MENU   0x10
#define GC_IN0_PAUSE  0x20
#define GC_IN0_SOUND  0x40
#define GC_IN0_A      0x80
/* in1: */
#define GC_IN1_B      0x01
#define GC_IN1_C      0x02
/* in2 (keys row 0xFFFF): */
#define GC_IN2_POWER  0x01
#define GC_IN2_D      0x02

/* 5-entry RGB palette (0=black .. 4=white), set by gamecom_init. */
extern uint8_t gamecom_palette[5][3];
/* GAMECOM_W*GAMECOM_H framebuffer of palette indices (0..4). */
extern uint8_t gamecom_fb[GAMECOM_W * GAMECOM_H];

/* internal_rom = 4KB internal BIOS (mapped at 0x1000).
   kernel_rom   = external "kernel" ROM (PDA firmware), up to 256KB; may be NULL.
   cart/cart_len= cartridge image (any supported size); may be NULL/0.
   Returns 0 on success, non-zero on a fatal argument error. */
int  gamecom_init(const uint8_t *internal_rom, int irom_len,
                  const uint8_t *kernel_rom, int krom_len,
                  const uint8_t *cart, int cart_len);

/* Run one video frame (≈92,575 CPU cycles across 200 scanlines). */
void gamecom_run_frame(void);

/* Set held buttons (active-low bytes; default 0xFF = nothing pressed). */
void gamecom_set_input_state(uint8_t in0, uint8_t in1, uint8_t in2);

/* Tap/release the touchscreen at screen pixel (x,y); pressed=0 lifts the stylus. */
void gamecom_set_stylus(int x, int y, int pressed);

/* Synthesize `n` signed-16-bit mono samples at `sample_rate` Hz from the live
 * SG0/SG1 wavetable channels + DAC. Advances internal phase; call once per frame
 * with n = sample_rate/fps. */
void gamecom_audio_mix(int16_t *out, int n, int sample_rate);

/* Serialize all machine + CPU state through a read-or-write callback (returns 1
 * on ok). saving!=0 writes; saving==0 reads then re-derives bank pointers.
 * ROM/BIOS pointers are NOT saved — they stay as gamecom_init() set them, so a
 * load is only valid within the same session (same firmware build + cart). */
typedef int (*gc_rw_fn)(void *ctx, void *data, uint32_t len);
int gamecom_state_rw(gc_rw_fn fn, void *ctx, int saving);

#endif /* GAMECOM_CORE_H */
