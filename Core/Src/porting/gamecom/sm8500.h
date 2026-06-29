// license:BSD-3-Clause
// copyright-holders:Wilbert Pol
//
// Freestanding C port of MAME's Sharp SM8500 CPU core (src/devices/cpu/sm8500).
// The opcode table (sm85ops.h) is reused VERBATIM from MAME; this file provides
// the thin C shim (state, bus accessors, interrupt/run loop) it needs.
//
// Bus + callbacks are supplied by the machine layer (gamecom_core.c):
//   gc_program_read / gc_program_write  -- the 16-bit program address space
//   gc_dma_cb  / gc_timer_cb            -- per-step DMA + timer hooks
#ifndef GAMECOM_SM8500_H
#define GAMECOM_SM8500_H

#include <stdint.h>

/* Interrupt lines (same ordering as MAME) */
enum {
	SM8500_ILL_INT  = 0,
	SM8500_DMA_INT  = 1,
	SM8500_TIM0_INT = 2,
	SM8500_EXT_INT  = 3,
	SM8500_UART_INT = 4,
	SM8500_LCDC_INT = 5,
	SM8500_TIM1_INT = 6,
	SM8500_CK_INT   = 7,
	SM8500_PIO_INT  = 8,
	SM8500_WDT_INT  = 9,
	SM8500_NMI_INT  = 10
};

#define SM8500_CLEAR_LINE  0
#define SM8500_ASSERT_LINE 1

/* --- supplied by the machine layer --- */
extern uint8_t gc_program_read(uint16_t addr);
extern void    gc_program_write(uint16_t addr, uint8_t data);
extern void    gc_dma_cb(uint8_t cycles);
extern void    gc_timer_cb(uint8_t cycles);

/* --- CPU API --- */
void     sm8500_reset(void);
/* run roughly `cycles` worth of execution; returns cycles actually consumed */
int      sm8500_execute(int cycles);
void     sm8500_set_input(int line, int state);
uint16_t sm8500_pc(void);   /* current PC, for tracing */

/* Serialize all CPU state through a read-or-write callback (returns 1 on ok).
 * The same field order runs for save and load; the caller's fn picks direction. */
int      sm8500_state_rw(int (*fn)(void *ctx, void *data, uint32_t len), void *ctx);

#endif /* GAMECOM_SM8500_H */
