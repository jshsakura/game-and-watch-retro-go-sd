/* IRQ accounting for the SNES device profiler — resident side.
 *
 * Ledger A (Core/Src/porting/snes/snes_profile.c) measures foreground DWT
 * cycles, and every one of its buckets is IRQ-INCLUSIVE: SysTick at 1 kHz and
 * the SAI half/complete callbacks land in whichever bucket happened to be open,
 * and they are CORRELATED with the phases (SAI with pacing, LTDC/DMA2D with
 * present), so they do not average out. Disabling interrupts to remove them is
 * not an option — audio pacing IS the interrupts, and the device would stop
 * behaving like the device.
 *
 * So they are counted instead, and the total is reported as the upper bound on
 * how much IRQ time inflates the buckets.
 *
 * WHY THIS LIVES IN Core/Inc AND THE COUNTERS IN stm32h7xx_it.c: these handlers
 * run under EVERY app, including the launcher and every other core. A counter
 * in the SNES overlay's BSS would be a write into whatever overlay happens to
 * be loaded — RAM_EMU is shared, one VMA, one core at a time (see CLAUDE.md).
 * Resident memory is the only correct home.
 *
 * Compiled only when SNES_DEVICE_PROFILE is defined GLOBALLY, which only the
 * diagnostic build does (Makefile: SNES_DEVICE_PROFILE=1 adds it to C_DEFS).
 * The release build sees an empty header. */
#pragma once

#ifdef SNES_DEVICE_PROFILE

#include <stdint.h>

/* Read DWT_CYCCNT directly so this header pulls in no dependency an interrupt
 * handler should not have. Armed by common_emu_enable_dwt_cycles(); before that
 * it reads 0 and the counters below are simply zero, which is harmless because
 * snes_profile_init() resets them after arming. */
#define SNES_PROF_DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004u)

extern volatile uint32_t snes_prof_irq_cycles;   /* cleared once per frame */
extern volatile uint32_t snes_prof_irq_count;

#endif /* SNES_DEVICE_PROFILE */
