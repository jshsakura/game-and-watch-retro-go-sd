/* Device-side 3-ledger frame profiler for the generic SNES core — interface.
 *
 * See snes_profile.c for the full design writeup. The short version, because
 * the shape of this API is a direct consequence of an adversarial review that
 * refuted the obvious design (tools/snes_survey/
 * snes_device_dwt_design_adversarial_review.md):
 *
 *   Ledger A  DWT foreground ACTIVE cycles, top-level and disjoint. Cumulative
 *             reads at loop boundaries; the caller never re-clears CYCCNT.
 *             IRQ-INCLUSIVE (see B_IRQ for the aggregate upper bound).
 *   Ledger B  exclusive attribution INSIDE the emulation/pcm outers: PPU
 *             inclusive, APU LLE exclusive, and the remainder — which is NOT
 *             "the 65816": it is CPU + DMA + event scheduler + spin bookkeeping.
 *   Ledger C  sleep-safe WALL time and audio deadlines. DWT cannot measure the
 *             pacing wait: it is __WFI(), the M7 gates the processor clock in
 *             sleep, and CYCCNT stops. Reading a near-zero DWT pacing bucket as
 *             "pacing is free, so we are compute-bound" is the exact wrong
 *             conclusion this ledger exists to prevent.
 *
 * Ledger A's `emu` and `pcm` buckets are OUTERS that contain Ledger B's values.
 * Never add a Ledger B value into a Ledger A sum — that double-counts.
 * DMA2D lifetime and audio-DMA ticks are async SIDE CHANNELS, never buckets.
 */
#pragma once

#ifdef SNES_DEVICE_PROFILE

#include <stdint.h>
#include <stdbool.h>
#include "common.h"     /* common_emu_get_dwt_cycles() */

/* ---- Ledger A: the marks the main loop takes, in loop order ---------------
 * Each is a CUMULATIVE DWT read; snes_profile_record() differences them. The
 * enum order IS the loop order, and the recorder asserts monotonicity. */
enum {
  SNES_PROF_M_FRAMECTL = 0,  /* wdog_refresh + common_emu_frame_loop()       */
  SNES_PROF_M_INPUT,         /* gamepad read + menu/turbo + read_snes_pad    */
  SNES_PROF_M_RENDER_ARM,    /* g_ppu_skip_render + PpuBeginDrawing arm      */
  SNES_PROF_M_EMU,           /* run_frame_events() — OUTER, contains ledger B*/
  SNES_PROF_M_PRESENT_KICK,  /* present_frame(): cache clean + DMA2D start,
                              * or the CPU scaler/copy on FIT/FULL           */
  SNES_PROF_M_PCM,           /* snes_pcm_submit() — OUTER, contains ledger B */
  SNES_PROF_M_PRESENT_TAIL,  /* present_frame_wait(): DMA2D poll TAIL only   */
  SNES_PROF_M_OVERLAY,       /* common_ingame_overlay()                      */
  SNES_PROF_M_SWAP,          /* lcd_swap()                                   */
  SNES_PROF_M_PACING,        /* audio-DMA pacing block (ACTIVE cycles only!) */
  SNES_PROF_M_COUNT
};

/* The mark array the main loop fills. A plain global (not a local) so the
 * SNES_PROF_MARK macro works anywhere in the loop body without threading a
 * pointer through; it is 40 B of overlay BSS in a diagnostic-only build. */
extern uint32_t snes_prof_mark[SNES_PROF_M_COUNT];

/* Take one cumulative read. Deliberately NOT a function call: one LDR from
 * DWT_CYCCNT plus one STR, so the probe is a handful of cycles and there are
 * only ten of them per frame (the review's "frame당 수십 회 이하" bound). */
#define SNES_PROF_MARK(m)  (snes_prof_mark[(m)] = common_emu_get_dwt_cycles())

/* ---- Ledger B: coarse scopes inside the outers ---------------------------
 * Two scopes only, and they are SIBLINGS, never nested: ppu_runLine() does not
 * reach the APU and snes_catchupApu() does not reach the PPU. That is what
 * lets us add them without a stack-based exclusive profiler. The depth
 * counters below exist to PROVE that claim every frame rather than assume it;
 * a nonzero snes_prof_b_err fails the whole run. */
extern uint32_t snes_prof_b_ppu_cyc;    /* PPU inclusive cycles, per frame    */
extern uint32_t snes_prof_b_ppu_calls;
extern uint32_t snes_prof_b_apu_cyc;    /* APU LLE exclusive cycles, per frame*/
extern uint32_t snes_prof_b_apu_calls;
extern uint32_t snes_prof_b_dma_cyc;    /* general-DMA cycles, APU-exclusive  */
extern uint32_t snes_prof_b_dma_calls;
extern int32_t  snes_prof_b_depth;      /* live scope depth; 0 at frame end   */
extern uint32_t snes_prof_b_depth_max;  /* worst depth seen in the window     */
extern uint32_t snes_prof_b_err;        /* nesting/underflow violations       */

static inline uint32_t snes_prof_scope_enter(void) {
  snes_prof_b_depth++;
  /* >1 means a Ledger B scope opened inside another one — the sibling
   * assumption is broken and every exclusive number below it is double-booked.
   * Count it; do not try to repair it. */
  if (snes_prof_b_depth > 1) snes_prof_b_err++;
  if ((uint32_t)snes_prof_b_depth > snes_prof_b_depth_max)
    snes_prof_b_depth_max = (uint32_t)snes_prof_b_depth;
  return common_emu_get_dwt_cycles();
}

static inline void snes_prof_scope_exit(uint32_t t0, uint32_t *acc, uint32_t *calls) {
  *acc += common_emu_get_dwt_cycles() - t0;   /* unsigned: wrap-safe */
  (*calls)++;
  snes_prof_b_depth--;
  if (snes_prof_b_depth < 0) snes_prof_b_err++;
}

/* Used by the generated copy of external/sm's snes.c (tools/snes_prof/
 * instrument.py). Kept as a macro so the instrumented file needs exactly one
 * textual substitution per call site and no local declarations of its own. */
#define SNES_PROF_PPU_CALL(expr)                                              \
  do {                                                                        \
    uint32_t snes_prof_t0__ = snes_prof_scope_enter();                        \
    (expr);                                                                   \
    snes_prof_scope_exit(snes_prof_t0__, &snes_prof_b_ppu_cyc,                \
                         &snes_prof_b_ppu_calls);                             \
  } while (0)

#define SNES_PROF_APU_SCOPE_ENTER()  snes_prof_scope_enter()
#define SNES_PROF_APU_SCOPE_EXIT(t0)                                          \
  snes_prof_scope_exit((t0), &snes_prof_b_apu_cyc, &snes_prof_b_apu_calls)

/* DMA drain scope: brackets $420B's synchronous general-DMA loop, which runs
 * INSIDE cpu_runOpcode. Like the CPU scope below (and unlike the PPU/APU sibling
 * scopes) it is snapshot-subtract and does NOT touch depth: the DMA can write
 * $2140-3 and re-enter snes_catchupApu()'s APU scope, which must stay depth
 * 0->1->0, not 1->2. It also takes back the APU delta so APU-driven-by-DMA is
 * not double-booked -- the result is DMA time exclusive of its APU work.
 * SNES_PROF_CPU_CALL subtracts it so cpu_only becomes the interpreter alone.
 * Used by the generated snes.c (instrument.py), so the statement (the whole
 * while loop) is the macro argument. */
#define SNES_PROF_DMA_CALL(stmt)                                              \
  do {                                                                        \
    uint32_t snes_prof_dt0__ = common_emu_get_dwt_cycles();                   \
    uint32_t snes_prof_da0__ = snes_prof_b_apu_cyc;                           \
    stmt;                                                                     \
    snes_prof_b_dma_cyc += (common_emu_get_dwt_cycles() - snes_prof_dt0__)    \
                         - (snes_prof_b_apu_cyc - snes_prof_da0__);           \
    snes_prof_b_dma_calls++;                                                  \
  } while (0)

/* ---- core_rem sub-split: the interpreter alone --------------------------
 * core_rem is "CPU + DMA + event scheduler + spin bookkeeping", and choosing
 * the next lever means knowing which of those it mostly is. cpu_runOpcode()
 * cannot use the depth-checked scope above, because it is NOT a sibling of the
 * APU scope: any $2140-3 access re-enters snes_catchupApu() from inside the
 * opcode (snes.c:321). So this is the snapshot-subtract form -- bracket the
 * call, then take back whatever the APU and PPU accumulators moved while we
 * were inside. Depth is deliberately untouched, so the sibling gate above
 * still means what it says.
 *
 * This one IS per-opcode, which every other probe in this file refuses to be.
 * That is a deliberate exception with a stated price: the dump prints the
 * call count and a probe estimate next to the bucket, and the bucket is only
 * ever used to answer "interpreter, or everything else?" -- a question whose
 * answer survives being wrong by the probe cost. It is not an A/B number. */
extern uint32_t snes_prof_b_cpu_cyc;
extern uint32_t snes_prof_b_cpu_calls;

/* ---- core_rem sub-split #2: spin bookkeeping ------------------------------
 * The 0726 SMK/SMW runs put core_rem - cpu_only - dma_only at 35-42% of
 * ACTIVE -- the single largest bucket -- and the residual's named suspects
 * are the spin learner (per REAL opcode: ring store + revisit scan in
 * spin_note) and HDMA (per line, invisible to dma_only which brackets only
 * the $420B drain). These two buckets name them. spin is per-opcode like
 * cpu_only, with the same stated price: the dump prints its probe estimate
 * and the bucket answers "learner, or scheduler?", nothing finer. spin_note
 * reaches no APU/PPU/DMA scope, so a plain bracket is exact. */
extern uint32_t snes_prof_b_spin_cyc;
extern uint32_t snes_prof_b_spin_calls;

#define SNES_PROF_SPIN_CALL(stmt)                                             \
  do {                                                                        \
    uint32_t snes_prof_st0__ = common_emu_get_dwt_cycles();                   \
    stmt;                                                                     \
    snes_prof_b_spin_cyc += common_emu_get_dwt_cycles() - snes_prof_st0__;    \
    snes_prof_b_spin_calls++;                                                 \
  } while (0)

/* HDMA scope: brackets dma_initHdma/dma_doHdma (per line, from
 * snes_handle_pos_stuff). Snapshot-subtract of the APU delta like
 * SNES_PROF_DMA_CALL: an HDMA channel targeting $2140-3 would re-enter
 * snes_catchupApu, and that work must stay booked as APU. Runs outside
 * cpu_runOpcode, so cpu_only needs no new subtraction. */
extern uint32_t snes_prof_b_hdma_cyc;
extern uint32_t snes_prof_b_hdma_calls;

#define SNES_PROF_HDMA_CALL(stmt)                                             \
  do {                                                                        \
    uint32_t snes_prof_ht0__ = common_emu_get_dwt_cycles();                   \
    uint32_t snes_prof_ha0__ = snes_prof_b_apu_cyc;                           \
    stmt;                                                                     \
    snes_prof_b_hdma_cyc += (common_emu_get_dwt_cycles() - snes_prof_ht0__)   \
                          - (snes_prof_b_apu_cyc - snes_prof_ha0__);          \
    snes_prof_b_hdma_calls++;                                                 \
  } while (0)

#define SNES_PROF_CPU_CALL(expr) ({                                           \
    uint32_t snes_prof_ct0__ = common_emu_get_dwt_cycles();                   \
    uint32_t snes_prof_ca0__ = snes_prof_b_apu_cyc;                           \
    uint32_t snes_prof_cp0__ = snes_prof_b_ppu_cyc;                           \
    uint32_t snes_prof_cd0__ = snes_prof_b_dma_cyc;                           \
    __typeof__(expr) snes_prof_cv__ = (expr);                                 \
    snes_prof_b_cpu_cyc += (common_emu_get_dwt_cycles() - snes_prof_ct0__)    \
                         - (snes_prof_b_apu_cyc - snes_prof_ca0__)            \
                         - (snes_prof_b_ppu_cyc - snes_prof_cp0__)            \
                         - (snes_prof_b_dma_cyc - snes_prof_cd0__);           \
    snes_prof_b_cpu_calls++;                                                  \
    snes_prof_cv__;                                                           \
  })

/* ---- Ledger C: sleep-safe wall clock -------------------------------------
 * TIM2 free-running at ~1 MHz. APB1 keeps clocking through plain Sleep, so
 * this counter sees the __WFI() pacing wait that DWT_CYCCNT cannot. NOT
 * SysTick/HAL_GetTick (stops in sleep, 1 ms resolution) and NOT dma_counter
 * (16.625 ms per tick — too coarse to subdivide a frame, though it is the
 * hardware reference the TIM2 rate is validated against over the window).
 * See snes_profile.c for why each alternative was rejected. Wraps every
 * ~71 min — deltas only, never absolute values. */
uint32_t snes_prof_wall_now(void);

/* ---- IRQ side ledger -----------------------------------------------------
 * Counters live in RESIDENT memory (stm32h7xx_it.c) because the handlers that
 * feed them run under every app; see Core/Inc/snes_prof_irq.h. */
#include "snes_prof_irq.h"

/* ---- API ------------------------------------------------------------------ */

/* Allocate the AHB sample pools, arm DWT, latch the SysTick period, and run
 * the busy-vs-WFI wall-clock sanity test. Call ONCE, after snes_init() (the
 * Apu's 66 KB ahb_malloc must already have happened so the pre-flight free-size
 * log tells the truth) and before the frame loop. audio_rate/audio_period are
 * SNES_AUDIO_RATE / SNES_AUDIO_SAMPLES — passed in rather than duplicated. */
void snes_profile_init(uint32_t audio_rate, uint32_t audio_period_samples);

/* Record one loop iteration and, once the window is full, perform the single
 * SD dump. Ledger B accumulators are read and reset here.
 *   active_base   DWT read taken at the very top of the iteration
 *   apu_cyc_in_emu  ledger-B APU cycles accumulated up to the end of
 *                   run_frame_events (the rest belong to the pcm outer)
 *   wall_frame    sleep-safe wall cycles for the whole iteration
 *   wall_pacing   sleep-safe wall cycles spent inside the pacing block
 *   dma_before    audio-DMA ticks already elapsed when pacing was ENTERED
 *                 (0 = we arrived early and had to wait; >=1 = deadline
 *                 already missed and the wait passed straight through)
 *   dma_frame     audio-DMA ticks consumed by this iteration (wall truth)
 *   wfi_count     __WFI() iterations executed in the pacing block */
void snes_profile_record(bool drawFrame, uint32_t active_base,
                         uint32_t apu_cyc_in_emu,
                         uint32_t wall_frame, uint32_t wall_pacing,
                         uint32_t dma_before, uint32_t dma_frame,
                         uint32_t wfi_count);

#else  /* !SNES_DEVICE_PROFILE — every hook compiles away to nothing */

#define SNES_PROF_MARK(m)            ((void)0)
#define SNES_PROF_APU_SCOPE_ENTER()  0u
#define SNES_PROF_APU_SCOPE_EXIT(t0) ((void)(t0))
#define SNES_PROF_CPU_CALL(expr)     (expr)
#define SNES_PROF_DMA_CALL(stmt)     do { stmt; } while (0)
#define SNES_PROF_SPIN_CALL(stmt)    do { stmt; } while (0)
#define SNES_PROF_HDMA_CALL(stmt)    do { stmt; } while (0)

#endif /* SNES_DEVICE_PROFILE */
