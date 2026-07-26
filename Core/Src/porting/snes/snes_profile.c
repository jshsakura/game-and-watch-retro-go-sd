/* Device-side 3-ledger frame profiler for the generic SNES core (LakeSnes).
 *
 * WHY THIS EXISTS
 * ---------------
 * Every SNES performance decision this tree has made was made on QEMU
 * instruction counts from tools/m7_qemu_rig — and that rig does not link
 * main_snes.c. It never saw the frame loop, present_frame(), the DMA2D wait,
 * the LCD swap or the audio pacing. "instructions / clock = fps" is only true
 * if the frame is compute-bound AND present/pacing cost zero, and nothing had
 * ever checked either half of that assumption on hardware. The render-frameskip
 * measurement that shipped on 0721 is what a device measurement is supposed to
 * prevent.
 *
 * WHAT THE OBVIOUS DESIGN GOT WRONG (and this one does not)
 * ---------------------------------------------------------
 * The first proposal was one flat six-bucket DWT split
 * (65816 / PPU / APU / present+DMA2D / pacing wait / rest). An adversarial
 * review refuted it — tools/snes_survey/snes_device_dwt_design_adversarial_
 * review.md. Three faults, each of which would have produced a confident wrong
 * answer rather than a visible failure:
 *
 *  1. DWT CANNOT MEASURE THE PACING WAIT. The wait is cpumon_sleep(), i.e.
 *     __WFI(). The Cortex-M7 gates the processor clock in sleep and CYCCNT is a
 *     PROCESSOR cycle counter, so the wait mostly does not appear in it. The
 *     bucket we cared about most would have read ~0 and we would have concluded
 *     "pacing is free, the frame is compute-bound" — the exact opposite of the
 *     truth if the loop is actually sitting on the audio deadline. Hence
 *     Ledger C and its SysTick-based wall clock, and hence the busy-vs-WFI
 *     sanity pair at init that PROVES on this device which of the two counters
 *     sees sleep.
 *  2. PPU AND APU ARE NOT TOP-LEVEL PHASES. ppu_runLine() is called from
 *     snes_handle_pos_stuff(), and snes_catchupApu() is called from inside
 *     cpu_runOpcode() via snes_readBBus()/RtlApuWrite() on any $2140-$2143
 *     access. They are CHILDREN of run_frame_events(). Adding an inclusive CPU
 *     timer to an inclusive APU timer double-books every catch-up. Hence
 *     Ledger B is a separate layer that re-partitions an OUTER, and is never
 *     summed into Ledger A.
 *  3. DMA2D IS ASYNCHRONOUS. present_frame() starts the copy and returns;
 *     snes_pcm_submit() runs while it is in flight; present_frame_wait() drains
 *     whatever tail is left. Charging the DMA2D lifetime to a present bucket
 *     counts the same wall interval twice, because the audio bucket already has
 *     it. Ledger A therefore contains only the CPU-side launch cost and the
 *     real poll tail; DMA2D and the audio DMA are SIDE CHANNELS.
 *
 * THE THREE LEDGERS
 * -----------------
 *  A  foreground ACTIVE cycles (DWT), top-level, disjoint, IRQ-inclusive.
 *  B  exclusive attribution inside the emu/pcm outers: PPU inclusive, APU LLE
 *     exclusive, remainder. The remainder is NOT "the 65816" — it is CPU + DMA
 *     + event scheduler + spin bookkeeping, and it is labelled that way.
 *  C  sleep-safe wall time, audio deadlines, and the async side channels.
 *
 * GATES (a run whose gates fail is not evidence)
 * ----------------------------------------------
 * Every one of these is printed in the dump, because the 32X profiler already
 * shipped two runs whose numbers were silently meaningless — one from a stale
 * build where the probes were never compiled in, one where a child scope was
 * not paused inside its parent and the same cycles landed in two buckets:
 *   - wall_sanity_busy   : the SysTick wall clock agrees with DWT while awake
 *   - wall_sanity_wfi    : does DWT see sleep on THIS device? (decides whether
 *                          the pacing ACTIVE bucket means anything at all)
 *   - wall_vs_dma        : the wall clock agrees with the audio DMA tick count
 *                          over the whole window (hardware cannot lie)
 *   - ledgerA_monotonic  : marks never go backwards (no nested CYCCNT clear,
 *                          no 12.6 s wrap inside a frame)
 *   - ledgerB_nesting    : scope depth never exceeded 1, ended each frame at 0
 *   - ledgerB_residual   : emu_outer - ppu - apu never went negative
 *   - probe_cost         : measured cost of one mark, x10 marks + record
 * scripts/check_snes_profile_wired.sh is the other half: it proves the probes
 * are in the BINARY, which is the failure the 32X run could not see from inside.
 *
 * PROBE DISCIPLINE
 * ----------------
 * Ten DWT reads per frame in Ledger A, and two coarse scopes in Ledger B whose
 * call counts are printed so a frequency surprise is visible. No per-opcode and
 * no per-DSP-tick brackets: an earlier APU experiment measured probe cost at
 * 0.38-8.37% of baseline, which is the same size as the effect being hunted.
 * The final A/B for any optimisation must still be run on a profiler-OFF
 * binary; this build exists to attribute cost, not to measure it.
 *
 * MEMORY
 * ------
 * Sample pools are ahb_calloc'd. The AHB dynamic pool is 87,904 B total; the
 * SNES core already takes 66,112 B of it in apu_init()'s ahb_malloc(sizeof(Apu))
 * — the only AHB allocation the core makes (Cpu/Cart/Ppu are DTCM heap, WRAM is
 * overlay BSS). That leaves 21,792 B, and this file asks for 10,240 B of it.
 * ahb_calloc does NOT fail soft: on overflow it asserts inside
 * gw_malloc.c:ahb_only_malloc and never returns, which is how the 32X profiler
 * hard-faulted a real device when Draw2FB had already eaten the pool. So the
 * _Static_assert below is the real guard, the pre-flight log is the only way to
 * see the truth if it ever stops being true, and ANY future feature that
 * allocates AHB inside the SNES core breaks this file first.
 */
#include "snes_profile.h"

#ifdef SNES_DEVICE_PROFILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <odroid_system.h>
#include "main.h"          /* wdog_refresh, HAL/CMSIS (SysTick, __WFI, __NOP) */
#include "common.h"
#include "gw_audio.h"      /* dma_counter */
#include "gw_malloc.h"
#include "odroid_display.h"
#include "snes/spin_skip.h" /* g_spin: the spin learner's own op counters */

#define SNES_PROF_VERSION      1
#define SNES_PROF_FRAMES       64u          /* ~1.07 s at 60 fps, then dump   */
#ifndef SNES_PROF_SKIP_FRAMES
#define SNES_PROF_SKIP_FRAMES  0u           /* frames discarded before the window
                                             * opens. Set it past whatever you are
                                             * waiting for -- e.g. 240 to profile the
                                             * N-SPC wire, which cannot swap before
                                             * frame 180. Both A/B arms must use the
                                             * same value. */
#endif
#define SNES_PROF_PATH         "/snes_dwt.txt"
#define SNES_PROF_DIAG_PATH    "/snes_diag.txt"

/* Measured 0721: 87,904 B AHB dynamic pool - 66,112 B for the Apu. Update this
 * number (and re-measure) if the SNES core ever takes AHB anywhere else; the
 * pre-flight line in snes_profile_init() prints the real value at boot so a
 * drift is visible before the assert-death rather than after it. */
#define SNES_PROF_AHB_BUDGET_BYTES  21792u

enum {
  /* ---- Ledger A: foreground active cycles (disjoint, IRQ-inclusive) ---- */
  BK_FRAMECTL = 0,
  BK_INPUT,
  BK_RENDER_ARM,
  BK_EMU,             /* OUTER: contains BK_PPU/BK_APU work                 */
  BK_PRESENT_KICK,
  BK_PCM,             /* OUTER: contains the APU top-up when LLE            */
  BK_PRESENT_TAIL,
  BK_OVERLAY,
  BK_SWAP,
  BK_PACING_ACT,      /* ACTIVE cycles in the pacing block, NOT the wait    */
  BK_ACTIVE_TOTAL,    /* whole iteration, DWT                               */
  /* ---- Ledger B: exclusive re-partition of the outers above ------------ */
  BK_PPU_INCL,
  BK_APU_EXCL,
  BK_CORE_REM,        /* emu_outer - ppu - apu_in_emu (CPU+DMA+events+spin) */
  BK_CPU_EXCL,        /* cpu_runOpcode() only; APU/PPU/DMA re-entry removed  */
  BK_DMA_EXCL,        /* $420B general-DMA drain, APU-exclusive             */
  BK_SPIN_EXCL,       /* spin learner: spin_note() per real opcode          */
  BK_HDMA_EXCL,       /* dma_initHdma/dma_doHdma per line, APU-exclusive    */
  /* ---- Ledger C: sleep-safe wall, deadlines, side channels ------------- */
  BK_WALL_FRAME,
  BK_WALL_PACING,
  BK_DMA_BEFORE,      /* audio ticks already elapsed at pacing entry        */
  BK_DMA_FRAME,       /* audio ticks consumed by the iteration              */
  BK_WFI,             /* __WFI() iterations in the pacing block             */
  BK_IRQ,             /* IRQ cycles charged during the iteration            */
  BK_COUNT
};

static const char *const bk_name[BK_COUNT] = {
  "framectl", "input", "rendarm", "emu*", "preskick", "pcm*",
  "prestail", "overlay", "swap", "pace_act", "ACTIVE",
  "ppu", "apu_lle", "core_rem", "cpu_only", "dma_only", "spin", "hdma",
  "WALL", "wall_pace", "dma_pre", "dma_tick", "wfi", "irq"
};

_Static_assert(2u * BK_COUNT * SNES_PROF_FRAMES * sizeof(uint32_t)
                 <= SNES_PROF_AHB_BUDGET_BYTES,
               "SNES profiler pools exceed the measured AHB headroom "
               "(shrink SNES_PROF_FRAMES) -- ahb_calloc asserts on overflow, "
               "it does not return NULL; see the 32X device Hardfault");

/* ---- storage -------------------------------------------------------------- */
uint32_t snes_prof_mark[SNES_PROF_M_COUNT];

uint32_t snes_prof_b_ppu_cyc, snes_prof_b_ppu_calls;
uint32_t snes_prof_b_apu_cyc, snes_prof_b_apu_calls;
uint32_t snes_prof_b_cpu_cyc, snes_prof_b_cpu_calls;
uint32_t snes_prof_b_dma_cyc, snes_prof_b_dma_calls;
uint32_t snes_prof_b_spin_cyc, snes_prof_b_spin_calls;
uint32_t snes_prof_b_hdma_cyc, snes_prof_b_hdma_calls;
int32_t  snes_prof_b_depth;
uint32_t snes_prof_b_depth_max;
uint32_t snes_prof_b_err;

static uint32_t n_warmup;      /* frames discarded before the window opens */
static uint32_t *pool_drawn;   /* [BK_COUNT * SNES_PROF_FRAMES] */
static uint32_t *pool_skip;
static uint64_t sum_drawn[BK_COUNT];
static uint64_t sum_skip[BK_COUNT];
static uint64_t call_ppu_sum, call_apu_sum, call_cpu_sum, call_dma_sum;
static uint64_t call_spin_sum, call_hdma_sum;
/* Spin learner benefit side. The `spin` bucket prices what the learner COSTS;
 * these price what it BUYS -- ops it replayed instead of interpreting. Deltas
 * of g_spin's own counters, so they cost one subtraction a frame. */
static uint64_t spin_real_sum, spin_virt_sum;
static uint64_t spin_real_last, spin_virt_last;
static bool     spin_gate_on_at_dump, spin_on_at_dump;
static uint32_t n_drawn, n_skip;
static bool     prof_active;   /* pools allocated                             */
static bool     prof_dumped;   /* strictly one-shot SD write                  */

/* gate counters */
static uint32_t g_mark_nonmonotonic;   /* frames where a mark went backwards  */
static uint32_t g_depth_nonzero;       /* frames that ended mid-scope         */
static uint32_t g_core_rem_negative;   /* frames where ppu+apu > emu_outer    */
static uint32_t g_cpu_over_core_rem;   /* frames where cpu_only > core_rem    */
static uint32_t g_dma_over_core_rem;   /* frames where dma_only > core_rem-cpu*/
static uint32_t g_dma_hist[4];         /* dma_before 0 / 1 / 2 / 3+           */

/* wall-clock sanity results, captured once at init */
static uint32_t g_sane_busy_dwt, g_sane_busy_wall;
static uint32_t g_sane_wfi_dwt,  g_sane_wfi_wall;
static uint32_t g_probe_cost_10; /* DWT cycles for 10 back-to-back marks      */

/* run identity */
static uint32_t g_audio_rate, g_audio_period;
static uint32_t g_ahb_free_before, g_ahb_free_after;

#define AT(pool, bk, i)  ((pool)[(bk) * SNES_PROF_FRAMES + (i)])

/* ---- Ledger C: the sleep-safe wall clock (TIM2) ---------------------------
 * TIM2: 32-bit, APB1, free-running, no interrupt, PSC set for ~1 MHz.
 *
 * WHY TIM2 AND NOT THE OBVIOUS CANDIDATES — all three were checked and all
 * three are wrong, so do not "simplify" this back to one of them:
 *
 *  - SysTick / HAL_GetTick(): STOPS in sleep. FCLK being "free-running" is an
 *    ARM architectural option, not a promise this part keeps, and 1 ms
 *    resolution could not resolve a 24.8 ms frame's phases anyway. Worth
 *    knowing the wider consequence: common_emu_frame_loop() computes elapsed
 *    time from get_elapsed_time(), which IS HAL_GetTick() — so the shared frame
 *    loop's own notion of elapsed time is not sleep-safe either. Audio pacing
 *    has been covering for that; a profiler built on the same clock would
 *    inherit the distortion it exists to measure.
 *  - dma_counter: hardware and immune to sleep, but one tick is a whole audio
 *    half-buffer — 266 samples at 16 kHz = 16.625 ms. It cannot subdivide a
 *    24.8 ms frame. It is still used here, as the WALL REFERENCE the TIM2 clock
 *    is checked against over the whole window (see wall_vs_dma), which is the
 *    job its resolution is right for.
 *  - RTC SSR (30.5 us) wraps every second; LPTIM collides with the wakeup path.
 *
 * TIM2 and TIM5 are both completely unused by this firmware (only TIM1 is
 * initialised, in main.c, for the charger/backlight path), so there is no
 * contention. TIM2 keeps counting through __WFI() because plain Sleep gates
 * only the CPU clock, not APB1. That premise is checked, not assumed: nothing
 * in Core/ or retro-go-stm32/ ever sets SLEEPDEEP or SCB->SCR, and the only
 * low-power entries in the tree are HAL_PWR_EnterSTANDBYMode() in gw_sleep.c
 * and gw_boot_rescue.c — both power-off paths, neither reachable from the
 * frame loop. cpumon_sleep() is therefore a plain Sleep-mode WFI. If a future
 * change introduces Stop mode into the play loop, APB1 stops and this clock
 * silently under-reports; the wall_vs_dma gate is what would catch it.
 *
 * The tick RATE is CALIBRATED at init against DWT during a busy spin rather
 * than derived from the RCC tree (D2PPRE1/TIMPRE make the APB1 timer clock
 * either 1x or 2x PCLK1, and the core changes clock at app start via
 * SystemClock_Config(1)). A wrong guess about the tree therefore cannot
 * silently rescale every wall number — the calibration absorbs it and the
 * busy gate reports it. Wraps every ~71 min at 1 MHz; only ever used as an
 * unsigned delta. */
static uint32_t g_tim_hz;      /* calibrated ticks per second */

uint32_t snes_prof_wall_now(void) {
  return TIM2->CNT;
}

static void snes_prof_wall_start(void) {
  __HAL_RCC_TIM2_CLK_ENABLE();
  /* First guess only; the calibration below is what the numbers actually use.
   * x2 because with any APB1 prescaler > 1 (which this clock tree uses) the
   * timer kernel clock is 2 x PCLK1. */
  uint32_t guess_hz = HAL_RCC_GetPCLK1Freq() * 2u;
  uint32_t psc      = guess_hz / 1000000u;
  if (psc == 0u) psc = 1u;

  TIM2->CR1  = 0;
  TIM2->PSC  = psc - 1u;
  TIM2->ARR  = 0xFFFFFFFFu;
  TIM2->EGR  = TIM_EGR_UG;      /* latch PSC into the active shadow register */
  TIM2->CNT  = 0;
  TIM2->DIER = 0;               /* free-running, no interrupt, no DMA        */
  TIM2->SR   = 0;
  TIM2->CR1  = TIM_CR1_CEN;
}

/* ---- init ----------------------------------------------------------------- */

/* Busy-vs-WFI sanity pair. Spends ~2 x 20 ms at load time (SD idle, loading
 * screen up) to answer the one question that decides how the pacing numbers
 * may be read: does DWT_CYCCNT advance while this device sleeps?
 *
 * Both loops are bounded by the WALL clock, so both cover the same real
 * interval. If wall is sound, busy_dwt/busy_wall ~= 1.0. If sleep gates the
 * processor clock (expected), wfi_dwt/wfi_wall is small — only the SysTick and
 * SAI ISR bodies that ran during those 20 ms. If instead the ratio comes back
 * near 1.0, either a debugger is attached with clocks held on in sleep, or the
 * wait never actually slept — and in that case the Ledger A pacing bucket IS
 * meaningful and the dump says so. */
static void snes_prof_wall_sanity(void) {
  /* Pass 1 — CALIBRATE, and simultaneously the busy gate. Spin a known number
   * of DWT cycles (DWT is trustworthy while awake; that is the one thing it is
   * unambiguously good at) and count TIM2 ticks over the same interval. The
   * resulting g_tim_hz is measured, not derived from D2PPRE1/TIMPRE, so a wrong
   * reading of the clock tree cannot silently rescale every wall number. */
  wdog_refresh();
  {
    const uint32_t span_cyc = SystemCoreClock / 20u;   /* ~50 ms */
    uint32_t w0 = snes_prof_wall_now();
    uint32_t d0 = common_emu_get_dwt_cycles();
    while (common_emu_get_dwt_cycles() - d0 < span_cyc) { __NOP(); }
    g_sane_busy_dwt  = common_emu_get_dwt_cycles() - d0;
    g_sane_busy_wall = snes_prof_wall_now() - w0;
    g_tim_hz = g_sane_busy_dwt
                 ? (uint32_t)(((uint64_t)g_sane_busy_wall * SystemCoreClock)
                              / g_sane_busy_dwt)
                 : 0u;
  }

  /* Pass 2 — does DWT see sleep on THIS device? Same shape as the real pacing
   * wait: __WFI() woken by whatever interrupt arrives first (SysTick at 1 kHz
   * guarantees progress). Bounded by the TIM2 clock, which is the whole point:
   * if TIM2 advanced 20 ms and DWT advanced far less, the processor clock was
   * gated and the Ledger A pacing bucket is ISR time, not wait time. */
  wdog_refresh();
  if (g_tim_hz) {
    const uint32_t span_tick = g_tim_hz / 50u;         /* ~20 ms */
    uint32_t w0 = snes_prof_wall_now();
    uint32_t d0 = common_emu_get_dwt_cycles();
    while (snes_prof_wall_now() - w0 < span_tick) { __WFI(); wdog_refresh(); }
    g_sane_wfi_dwt  = common_emu_get_dwt_cycles() - d0;
    g_sane_wfi_wall = snes_prof_wall_now() - w0;
  }
  wdog_refresh();

  /* Cost of the Ledger A probe itself: ten back-to-back marks, which is
   * exactly what one frame pays. Reported as a fraction of the frame so the
   * intrusion claim is measured on the device instead of assumed. */
  {
    uint32_t d0 = common_emu_get_dwt_cycles();
    SNES_PROF_MARK(0); SNES_PROF_MARK(1); SNES_PROF_MARK(2); SNES_PROF_MARK(3);
    SNES_PROF_MARK(4); SNES_PROF_MARK(5); SNES_PROF_MARK(6); SNES_PROF_MARK(7);
    SNES_PROF_MARK(8); SNES_PROF_MARK(9);
    g_probe_cost_10 = common_emu_get_dwt_cycles() - d0;
  }
}

void snes_profile_init(uint32_t audio_rate, uint32_t audio_period_samples) {
  g_audio_rate   = audio_rate;
  g_audio_period = audio_period_samples;

  common_emu_enable_dwt_cycles();
  snes_prof_wall_start();

  /* Pre-flight, BEFORE the allocation: ahb_only_malloc() asserts on overflow
   * instead of returning NULL, so a prof_active==false path can never be
   * reached and this line is the only way to ever see the real numbers. The
   * load-time /snes_diag.txt is already written by main_snes.c at this point,
   * so append; both are boot-adjacent, SD-idle, one-shot writes. */
  g_ahb_free_before = (uint32_t)ahb_get_free_size();
  {
    FILE *df = fopen(SNES_PROF_DIAG_PATH, "ab");
    if (df) {
      fprintf(df, "profiler init: ahb_free_before=%u need=%u budget_assumed=%u\n",
              (unsigned)g_ahb_free_before,
              (unsigned)(2u * BK_COUNT * SNES_PROF_FRAMES * sizeof(uint32_t)),
              (unsigned)SNES_PROF_AHB_BUDGET_BYTES);
      fclose(df);
    }
  }

  /* ahb_ONLY_malloc, not ahb_calloc. ahb_calloc() tries ram_malloc() FIRST
   * (gw_malloc.c:98-107) and ram_malloc() asserts on ram_start == 0. The SNES
   * core never sets ram_start -- main_snes.c has no reference to it at all --
   * and rg_emulators.c:1879 clears the cursor to the "unowned" sentinel right
   * before launching a game. So the first ahb_calloc() here killed the device
   * with `Assertion "ram_start != 0" failed ... ram_malloc` before a single
   * frame ran. md32x hit exactly this and left the warning at
   * main_md32x.c:511 ("ahb_calloc() tries ram_malloc FIRST ... first device
   * boot died"); this file's own comment above already names ahb_only_malloc
   * as the intended allocator, so this is the call it always meant.
   * ahb_only_malloc does NOT return NULL on overflow -- it asserts -- which is
   * why the pre-flight above logs the free size before we get here. */
  {
    const size_t n = (size_t)BK_COUNT * SNES_PROF_FRAMES * sizeof(uint32_t);
    pool_drawn = ahb_only_malloc(n);
    pool_skip  = ahb_only_malloc(n);
    if (pool_drawn) memset(pool_drawn, 0, n);
    if (pool_skip)  memset(pool_skip,  0, n);
  }
  prof_active = (pool_drawn != NULL && pool_skip != NULL);
  g_ahb_free_after = (uint32_t)ahb_get_free_size();

  snes_prof_wall_sanity();

  {
    FILE *df = fopen(SNES_PROF_DIAG_PATH, "ab");
    if (df) {
      fprintf(df, "profiler init: active=%d ahb_free_after=%u tim_hz=%u "
                  "sanity busy=%u/%u wfi=%u/%u probe10=%u\n",
              (int)prof_active, (unsigned)g_ahb_free_after, (unsigned)g_tim_hz,
              (unsigned)g_sane_busy_dwt, (unsigned)g_sane_busy_wall,
              (unsigned)g_sane_wfi_dwt,  (unsigned)g_sane_wfi_wall,
              (unsigned)g_probe_cost_10);
      fclose(df);
    }
  }

  /* The sanity pair scribbled on the mark array; leave a clean slate. */
  memset(snes_prof_mark, 0, sizeof(snes_prof_mark));
  snes_prof_b_ppu_cyc = snes_prof_b_ppu_calls = 0;
  snes_prof_b_apu_cyc = snes_prof_b_apu_calls = 0;
  snes_prof_b_cpu_cyc = snes_prof_b_cpu_calls = 0;
  snes_prof_b_dma_cyc = snes_prof_b_dma_calls = 0;
  snes_prof_b_spin_cyc = snes_prof_b_spin_calls = 0;
  snes_prof_b_hdma_cyc = snes_prof_b_hdma_calls = 0;
  snes_prof_b_depth = 0;
  snes_prof_b_depth_max = 0;
  snes_prof_b_err = 0;
  snes_prof_irq_cycles = 0;
  snes_prof_irq_count = 0;
}

/* ---- percentile helpers (32X pattern) ------------------------------------- */
static int u32cmp(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

static uint32_t pct_sorted(const uint32_t *s, uint32_t n, uint32_t p) {
  if (n == 0) return 0;
  uint32_t i = (n * p) / 100u;
  if (i >= n) i = n - 1u;
  return s[i];
}

/* x10 percentage helper: returns tenths of a percent, saturating. */
static unsigned pct10(uint64_t part, uint64_t whole) {
  return whole ? (unsigned)((part * 1000u) / whole) : 0u;
}

/* Sorts the slice IN PLACE — safe, the data is dead after the one dump.
 * %u only, never %lu: this build's nano.specs printf does not parse the 'l'
 * length modifier and emits the literal characters "lu" (observed on device,
 * 0720 /32x_dwt.txt). Every value here fits unsigned int at this window size. */
static void emit_bucket(FILE *f, uint32_t bk, char kind,
                        uint32_t *pool, uint32_t n, uint64_t sum,
                        uint64_t denom) {
  if (n == 0) {
    fprintf(f, "  %-9s[%c] n=0\n", bk_name[bk], kind);
    return;
  }
  uint32_t *slice = pool + bk * SNES_PROF_FRAMES;
  qsort(slice, n, sizeof(uint32_t), u32cmp);
  fprintf(f, "  %-9s[%c] avg=%u p50=%u p90=%u p95=%u p99=%u",
          bk_name[bk], kind,
          (unsigned)(sum / n),
          (unsigned)pct_sorted(slice, n, 50),
          (unsigned)pct_sorted(slice, n, 90),
          (unsigned)pct_sorted(slice, n, 95),
          (unsigned)pct_sorted(slice, n, 99));
  /* denom == 0 means "this bucket is a count, a share of nothing" — print no
   * percentage rather than a 0.0% that reads like a measurement. */
  if (denom) {
    unsigned share = pct10(sum, denom);
    fprintf(f, "  %u.%u%%", share / 10u, share % 10u);
  }
  fprintf(f, "\n");
}

/* ---- the one dump --------------------------------------------------------- */
static void snes_profile_dump(void) {
  extern uint32_t SystemCoreClock;

  const uint32_t frames = n_drawn + n_skip;
  /* Two independent wall measurements, deliberately kept apart:
   *   TIM2  — per-frame resolution, used for every per-frame percentile below.
   *   audio DMA ticks — 16.625 ms granularity, far too coarse per frame, but it
   *   is HARDWARE and immune to sleep, to our reading of the clock tree and to
   *   the profiler itself. Over a 64-frame window it is exact to ~0.1%, so it
   *   is the REFERENCE that TIM2 is checked against (wall_vs_dma), and it is
   *   what fps is computed from. Never the other way round. */
  const uint64_t dma_ticks    = sum_drawn[BK_DMA_FRAME] + sum_skip[BK_DMA_FRAME];
  const uint64_t tick_us      = g_audio_rate
                                  ? (1000000ull * g_audio_period) / g_audio_rate
                                  : 0ull;
  const uint64_t wall_us_dma  = dma_ticks * tick_us;
  const uint64_t wall_tick_sum= sum_drawn[BK_WALL_FRAME] + sum_skip[BK_WALL_FRAME];
  const uint64_t wall_us_tim  = g_tim_hz
                                  ? (wall_tick_sum * 1000000ull) / g_tim_hz
                                  : 0ull;
  /* Convert the DMA-referenced wall time into core cycles so it can be compared
   * with the DWT sums directly. */
  const uint64_t wall_cyc_dma = ((uint64_t)SystemCoreClock * wall_us_dma) / 1000000ull;
  const uint64_t active_sum   = sum_drawn[BK_ACTIVE_TOTAL] + sum_skip[BK_ACTIVE_TOTAL];
  const uint64_t apu_sum      = sum_drawn[BK_APU_EXCL] + sum_skip[BK_APU_EXCL];
  const uint64_t irq_sum      = sum_drawn[BK_IRQ] + sum_skip[BK_IRQ];

  /* fps from the DMA clock, x10. */
  const unsigned fps10 = wall_us_dma
                           ? (unsigned)((10000000ull * frames) / wall_us_dma)
                           : 0u;
  const unsigned drawn_fps10 = wall_us_dma
                           ? (unsigned)((10000000ull * n_drawn) / wall_us_dma)
                           : 0u;

  odroid_audio_mute(true);
  wdog_refresh();

  FILE *f = fopen(SNES_PROF_PATH, "wb");
  wdog_refresh();
  if (f == NULL) { odroid_audio_mute(false); return; }

  fprintf(f, "=== SNES device frame profile (3-ledger) v%d ===\n", SNES_PROF_VERSION);
  fprintf(f, "clk=%u Hz  oc_user=%u  scaling=%d  window=%u frames "
             "(drawn=%u skip=%u)\n",
          (unsigned)SystemCoreClock,
          (unsigned)odroid_settings_cpu_oc_level_get(),
          (int)odroid_display_get_scaling_mode(),
          (unsigned)frames, (unsigned)n_drawn, (unsigned)n_skip);
  fprintf(f, "audio: rate=%u Hz period=%u samples = %u us/tick   ahb_free %u->%u\n",
          (unsigned)g_audio_rate, (unsigned)g_audio_period, (unsigned)tick_us,
          (unsigned)g_ahb_free_before, (unsigned)g_ahb_free_after);
  fprintf(f, "NOTE: Ledger A is IRQ-INCLUSIVE. 'emu*' and 'pcm*' are OUTERS -- "
             "Ledger B re-partitions them, never add the two together.\n");
  fprintf(f, "warmup: %u frames discarded before the window opened "
             "(SNES_PROF_SKIP_FRAMES). A/B arms MUST match on this.\n",
          (unsigned)SNES_PROF_SKIP_FRAMES);
  /* The dump has to say which A/B arm produced it. SNES_LOAD_DIAG is the usual
   * source of that line and it is mutually exclusive with this profiler
   * (Makefile:103), so without this the two arms are indistinguishable once the
   * files are off the card -- which is exactly how two identical binaries got
   * published as an A/B pair and three runs of the same firmware were read as a
   * comparison. */
#ifdef SNES_SMW_HLE_PRODUCT
  {
    extern int g_wire_enable, g_wire_on;
    extern const char *g_wire_variant;
    fprintf(f, "wire: BUILT-IN  enable=%d on=%d variant=%s\n",
            g_wire_enable, g_wire_on, g_wire_variant ? g_wire_variant : "-");
  }
#else
  fprintf(f, "wire: NOT COMPILED (pure LLE reference arm)\n");
#endif
  wdog_refresh();

  /* ---- gates ---- */
  {
    /* TIM2 was calibrated to ~1 MHz. A big miss means the PSC guess and the
     * real APB1 timer clock disagree by more than the calibration can be
     * trusted to absorb -- or that TIM2 is not running at all. */
    unsigned tim_err10 = g_tim_hz
        ? (unsigned)(((g_tim_hz > 1000000u ? g_tim_hz - 1000000u
                                           : 1000000u - g_tim_hz) * 1000ull) / 1000000u)
        : 1000u;
    /* Expected DWT cycles for the WFI interval had the clock never been gated.
     * ratio << 100% => sleep is invisible to DWT, which is the expected result
     * and the reason Ledger C exists. */
    uint64_t wfi_expect_cyc = g_tim_hz
        ? ((uint64_t)g_sane_wfi_wall * SystemCoreClock) / g_tim_hz : 0ull;
    unsigned wfi_r = wfi_expect_cyc
        ? (unsigned)(((uint64_t)g_sane_wfi_dwt * 1000ull) / wfi_expect_cyc) : 0u;
    /* wall vs dma: relative error in tenths of a percent, either direction */
    uint64_t hi = wall_us_tim > wall_us_dma ? wall_us_tim : wall_us_dma;
    uint64_t lo = wall_us_tim > wall_us_dma ? wall_us_dma : wall_us_tim;
    unsigned wall_err10 = hi ? (unsigned)(((hi - lo) * 1000ull) / hi) : 0u;
    unsigned probe_share10 = active_sum && frames
        ? pct10((uint64_t)g_probe_cost_10 * frames, active_sum) : 0u;

    fprintf(f, "\n--- gates (a run whose gates fail is not evidence) ---\n");
    fprintf(f, "  tim2_cal    %u Hz (target 1000000), err=%u.%u%%  %s\n",
            (unsigned)g_tim_hz, tim_err10 / 10u, tim_err10 % 10u,
            (g_tim_hz && tim_err10 <= 50u) ? "PASS" : "FAIL");
    fprintf(f, "  wall_wfi    dwt/expected=%u.%u%%  %s\n",
            wfi_r / 10u, wfi_r % 10u,
            (wfi_r < 500) ? "SLEEP-BLIND (DWT does NOT count through WFI -- "
                            "pace_act is ISR time, NOT the wait; use wall_pace)"
                          : "SLEEP-VISIBLE (DWT counted through WFI -- pace_act "
                            "is real wait time; check no debugger is attached)");
    fprintf(f, "  wall_vs_dma err=%u.%u%%  %s  (tim2=%u us, audio-dma=%u us)\n",
            wall_err10 / 10u, wall_err10 % 10u, (wall_err10 <= 20) ? "PASS" : "FAIL",
            (unsigned)wall_us_tim, (unsigned)wall_us_dma);
    fprintf(f, "  ledgerA_mono violations=%u  %s\n",
            (unsigned)g_mark_nonmonotonic, g_mark_nonmonotonic ? "FAIL" : "PASS");
    fprintf(f, "  ledgerB_nest depth_max=%u err=%u frame_end_nonzero=%u  %s\n",
            (unsigned)snes_prof_b_depth_max, (unsigned)snes_prof_b_err,
            (unsigned)g_depth_nonzero,
            (snes_prof_b_depth_max <= 1 && snes_prof_b_err == 0 && g_depth_nonzero == 0)
              ? "PASS" : "FAIL");
    fprintf(f, "  ledgerB_resid negative_core_rem_frames=%u  %s\n",
            (unsigned)g_core_rem_negative, g_core_rem_negative ? "FAIL" : "PASS");
    fprintf(f, "  probe_cost  %u cyc/frame for 10 marks = %u.%u%% of ACTIVE "
               "(informational; final A/B must be profiler-OFF)\n",
            (unsigned)g_probe_cost_10, probe_share10 / 10u, probe_share10 % 10u);
    fprintf(f, "  irq_share   %u.%u%% of ACTIVE  "
               "(upper bound on how much IRQ inflates every Ledger A bucket;\n"
               "               disabling interrupts to remove it would change "
               "audio pacing itself, so it is reported, never suppressed)\n",
            pct10(irq_sum, active_sum) / 10u, pct10(irq_sum, active_sum) % 10u);
  }
  wdog_refresh();

  /* ---- Ledger C headline: is fps compute-bound or deadline-bound? ---- */
  fprintf(f, "\n--- Ledger C: wall / deadline (the fps ceiling question) ---\n");
  fprintf(f, "  emulated fps=%u.%u   drawn fps=%u.%u   wall=%u us over %u frames\n",
          fps10 / 10u, fps10 % 10u, drawn_fps10 / 10u, drawn_fps10 % 10u,
          (unsigned)wall_us_dma, (unsigned)frames);
  {
    /* Clamped: ACTIVE can exceed WALL if the IRQ ledger double-charges or the
     * wall reference is short, and an unsigned 1000-x would print a garbage
     * 400%-idle instead of showing that something is wrong. */
    unsigned busy10 = pct10(active_sum, wall_cyc_dma);
    unsigned idle10 = busy10 >= 1000u ? 0u : 1000u - busy10;
    fprintf(f, "  cpu busy = ACTIVE/WALL = %u.%u%%   -> idle+sleep = %u.%u%%%s\n",
            busy10 / 10u, busy10 % 10u, idle10 / 10u, idle10 % 10u,
            busy10 >= 1000u ? "   <-- ACTIVE >= WALL: check the gates above" : "");
  }
  fprintf(f, "  deadline advance at pacing entry:  0=%u  1=%u  2=%u  3+=%u\n",
          (unsigned)g_dma_hist[0], (unsigned)g_dma_hist[1],
          (unsigned)g_dma_hist[2], (unsigned)g_dma_hist[3]);
  fprintf(f, "  (0 = arrived early and WAITED -> slack exists at this scene's mean.\n"
             "   >=1 = the audio deadline had already passed -> that frame overran\n"
             "   and LLE does not recover the missed period. A large mean wait and a\n"
             "   heavy p95/p99 tail can and do coexist -- judge on the tail too.)\n");
  wdog_refresh();

  /* ---- the tables ---- */
  fprintf(f, "\n--- Ledger A: foreground ACTIVE cycles (DWT, IRQ-inclusive) ---\n");
  fprintf(f, "  [D]=drawn frames, [S]=skipped; %% is of that pool's ACTIVE total\n");
  for (uint32_t bk = BK_FRAMECTL; bk <= BK_ACTIVE_TOTAL; bk++) {
    emit_bucket(f, bk, 'D', pool_drawn, n_drawn, sum_drawn[bk], sum_drawn[BK_ACTIVE_TOTAL]);
    emit_bucket(f, bk, 'S', pool_skip,  n_skip,  sum_skip[bk],  sum_skip[BK_ACTIVE_TOTAL]);
  }
  wdog_refresh();

  fprintf(f, "\n--- Ledger B: inside emu*/pcm* (exclusive; %% is of ACTIVE) ---\n");
  fprintf(f, "  ppu_calls/frame=%u  apu_calls/frame=%u  cpu_calls/frame=%u\n",
          (unsigned)(frames ? call_ppu_sum / frames : 0),
          (unsigned)(frames ? call_apu_sum / frames : 0),
          (unsigned)(frames ? call_cpu_sum / frames : 0));
  fprintf(f, "  core_rem is CPU + DMA + event scheduler + spin bookkeeping -- "
             "it is NOT 'the 65816'.\n");
  /* cpu_only breaks that sentence open. It is bracketed per opcode, so unlike
   * every other bucket here its probe cost is NOT negligible -- roughly two DWT
   * reads per call, and there are thousands of calls a frame. The estimate
   * below is printed so the bucket is read with it, never without: subtract it
   * before quoting cpu_only, and never quote cpu_only as a wall share. This
   * bucket exists to answer ONE question -- is core_rem mostly the interpreter
   * or mostly everything else -- and the answer only has to survive being off
   * by its own probe cost. */
  {
    uint32_t cpu_pf   = (uint32_t)(frames ? sum_drawn[BK_CPU_EXCL] / (n_drawn ? n_drawn : 1) : 0);
    uint32_t calls_pf = (unsigned)(frames ? call_cpu_sum / frames : 0);
    uint32_t probe_pf = calls_pf * (g_probe_cost_10 / 10u) * 2u;
    fprintf(f, "  cpu_only = cpu_runOpcode() alone, APU/PPU re-entry subtracted.\n"
               "    probe estimate %lu cyc/frame (%lu calls x 2 marks x %lu cyc);"
               " drawn cpu_only avg %lu -> ~%lu corrected\n",
            (unsigned long)probe_pf, (unsigned long)calls_pf,
            (unsigned long)(g_probe_cost_10 / 10u), (unsigned long)cpu_pf,
            (unsigned long)(cpu_pf > probe_pf ? cpu_pf - probe_pf : 0));
    fprintf(f, "  cpu_over_core_rem frames=%u  %s\n",
            (unsigned)g_cpu_over_core_rem, g_cpu_over_core_rem ? "FAIL" : "PASS");
    fprintf(f, "  dma_only = $420B general-DMA drain, APU-exclusive; "
               "dma_calls/frame=%u.\n"
               "  spin = spin_note() per real opcode, spin_calls/frame=%u"
               " (probe ~%lu cyc/frame -- subtract before quoting).\n"
               "  hdma = dma_initHdma/doHdma per line, APU-exclusive; "
               "hdma_calls/frame=%u.\n"
               "  core_rem - cpu_only - dma_only - spin - hdma = event scheduler.\n"
               "  subsplit_over_core_rem frames=%u  %s\n",
            (unsigned)(frames ? call_dma_sum / frames : 0),
            (unsigned)(frames ? call_spin_sum / frames : 0),
            (unsigned long)((frames ? call_spin_sum / frames : 0) * (g_probe_cost_10 / 10u) * 2u),
            (unsigned)(frames ? call_hdma_sum / frames : 0),
            (unsigned)g_dma_over_core_rem, g_dma_over_core_rem ? "FAIL" : "PASS");
  }
  for (uint32_t bk = BK_PPU_INCL; bk <= BK_HDMA_EXCL; bk++) {
    emit_bucket(f, bk, 'D', pool_drawn, n_drawn, sum_drawn[bk], sum_drawn[BK_ACTIVE_TOTAL]);
    emit_bucket(f, bk, 'S', pool_skip,  n_skip,  sum_skip[bk],  sum_skip[BK_ACTIVE_TOTAL]);
  }
  wdog_refresh();

  /* ---- spin-skip breakeven, from THIS run --------------------------------
   * The `spin` bucket above is only the COST side. The learner earns its keep
   * by replaying loop iterations instead of interpreting them, so the verdict
   * needs both, and both are now measured: cost is the bucket, benefit is
   * ops_virtual x what an interpreted opcode actually costs here. Averages are
   * over ALL recorded frames so the op counters and the cycle buckets share a
   * denominator. Probe cycles are removed from both sides before dividing --
   * spin and cpu_only are the two per-opcode-bracketed buckets, so quoting
   * either raw would flatter the one with more calls. */
  {
    uint64_t fr = frames ? frames : 1;
    uint64_t probe1 = g_probe_cost_10 / 10u;
    uint64_t real_pf = spin_real_sum / fr;
    uint64_t virt_pf = spin_virt_sum / fr;
    uint64_t cpu_calls_pf = call_cpu_sum / fr;

    uint64_t spin_raw_pf = (sum_drawn[BK_SPIN_EXCL] + sum_skip[BK_SPIN_EXCL]) / fr;
    uint64_t cpu_raw_pf  = (sum_drawn[BK_CPU_EXCL]  + sum_skip[BK_CPU_EXCL])  / fr;
    uint64_t spin_probe  = real_pf * 2u * probe1;
    uint64_t cpu_probe   = cpu_calls_pf * 2u * probe1;
    uint64_t spin_net_pf = spin_raw_pf > spin_probe ? spin_raw_pf - spin_probe : 0;
    uint64_t cpu_net_pf  = cpu_raw_pf  > cpu_probe  ? cpu_raw_pf  - cpu_probe  : 0;
    uint64_t per_op      = cpu_calls_pf ? cpu_net_pf / cpu_calls_pf : 0;
    uint64_t benefit_pf  = virt_pf * per_op;

    fprintf(f, "\n--- spin-skip breakeven (cost bucket vs replay benefit) ---\n");
    fprintf(f, "  gate_on=%d pattern_on=%d   ops/frame: real=%lu virtual=%lu (replayed %lu%% of all ops)\n",
            spin_gate_on_at_dump, spin_on_at_dump,
            (unsigned long)real_pf, (unsigned long)virt_pf,
            (unsigned long)((real_pf + virt_pf) ? (100u * virt_pf) / (real_pf + virt_pf) : 0));
    fprintf(f, "  cost    = %lu cyc/frame (spin bucket, probe-corrected)\n",
            (unsigned long)spin_net_pf);
    fprintf(f, "  benefit = %lu cyc/frame (%lu virtual ops x %lu cyc/op interpreted)\n",
            (unsigned long)benefit_pf, (unsigned long)virt_pf, (unsigned long)per_op);
    if (benefit_pf >= spin_net_pf)
      fprintf(f, "  NET +%lu cyc/frame => the learner PAYS for itself on this ROM/scene.\n",
              (unsigned long)(benefit_pf - spin_net_pf));
    else
      fprintf(f, "  NET -%lu cyc/frame => the learner COSTS more than it saves here;\n"
                 "      an OFF arm (SNES_SPIN_SKIP_DEFAULT=false, or a spin_table entry)\n"
                 "      is the A/B to run. Zelda already went that way at 25%% skip.\n",
              (unsigned long)(spin_net_pf - benefit_pf));
    fprintf(f, "  Not counted on the cost side: the replay branch itself in run_dots\n"
               "  (it lives in the scheduler residue), so NET is an UPPER bound on the\n"
               "  learner's value. A profiler-OFF device A/B remains the final word.\n");
  }
  wdog_refresh();

  fprintf(f, "\n--- Ledger C detail: WALL/wall_pace are TIM2 ticks (~1 us), "
             "dma_*/wfi are counts, irq is DWT cycles ---\n");
  for (uint32_t bk = BK_WALL_FRAME; bk < BK_COUNT; bk++) {
    /* Deliberately per-bucket denominators: a percentage only means something
     * when the numerator and the whole are in the same unit. Counts get none. */
    uint64_t denom = (bk == BK_WALL_FRAME || bk == BK_WALL_PACING) ? wall_tick_sum
                   : (bk == BK_IRQ)                                ? active_sum
                                                                   : 0ull;
    emit_bucket(f, bk, 'D', pool_drawn, n_drawn, sum_drawn[bk], denom);
    emit_bucket(f, bk, 'S', pool_skip,  n_skip,  sum_skip[bk],  denom);
  }
  wdog_refresh();

  /* ---- the judgement the run exists to serve ---- */
  {
    /* Required wall-cost saving for a target fps, from the fps MEASURED here:
     *   saving = 1 - fps_base/fps_target.
     * Printed for +3/+5/+10 because the success line moved from +10 to +5 on
     * 0721 and that halves the bar -- 40.3 fps needs 19.9% for +10 but only
     * 11.0% for +5. A number that clears one line and not the other is the
     * normal case, so both are printed rather than one verdict. */
    unsigned need3 = 0, need5 = 0, need10 = 0;
    if (fps10) {
      need3  = (unsigned)(1000u - (1000ull * fps10) / (fps10 + 30u));
      need5  = (unsigned)(1000u - (1000ull * fps10) / (fps10 + 50u));
      need10 = (unsigned)(1000u - (1000ull * fps10) / (fps10 + 100u));
    }
    unsigned apu_of_active = pct10(apu_sum, active_sum);
    unsigned apu_of_wall   = pct10(apu_sum, wall_cyc_dma);

    fprintf(f, "\n--- APU-wire judgement lines (recomputed from THIS run) ---\n");
    fprintf(f, "  required wall-cost saving:  +3fps=%u.%u%%  +5fps=%u.%u%%  +10fps=%u.%u%%\n",
            need3 / 10u, need3 % 10u, need5 / 10u, need5 % 10u, need10 / 10u, need10 % 10u);
    fprintf(f, "  measured APU LLE exclusive: %u.%u%% of ACTIVE, %u.%u%% of WALL\n",
            apu_of_active / 10u, apu_of_active % 10u,
            apu_of_wall / 10u, apu_of_wall % 10u);
    fprintf(f, "  recoverable upper bound = APU_LLE_exclusive\n"
               "                          + the CPU-side $2140-3 port/catchup cost the wire removes\n"
               "                          - the wire's own run cost.\n"
               "  Below the +5 line => NO-GO for +5 on the APU axis alone. Above it is\n"
               "  NOT a GO: LLE cost becomes wire cost, and a GO still needs an exact\n"
               "  port transcript, multi-scene traces, and a profiler-OFF device A/B.\n");
  }

  wdog_refresh();
  fclose(f);
  wdog_refresh();
  odroid_audio_mute(false);
}

/* ---- record --------------------------------------------------------------- */
void snes_profile_record(bool drawFrame, uint32_t active_base,
                         uint32_t apu_cyc_in_emu,
                         uint32_t wall_frame, uint32_t wall_pacing,
                         uint32_t dma_before, uint32_t dma_frame,
                         uint32_t wfi_count) {
  if (!prof_active || prof_dumped) return;

  /* Warm-up skip. The window has to OPEN AFTER whatever it is meant to measure
   * has started, and for the N-SPC audio HLE that is not frame 0: the wire
   * cannot swap before frame 180 (`NSPC_SWAP_MIN_FRAME` 120, then `frame % 60`,
   * then a two-check stability streak), while this window closes at frame 64.
   * With no skip, a wire-ON build profiles 64 frames of pure LLE and reports
   * numbers identical to the wire-OFF arm -- which is exactly what the first
   * A/B attempt produced, and it looked like "the wire does nothing" rather
   * than "the measurement ended before the wire started".
   * Both arms must use the SAME skip so the comparison stays honest. */
  if (n_warmup < SNES_PROF_SKIP_FRAMES) {
    n_warmup++;
    /* Returning early is NOT enough: Ledger B's accumulators keep filling during
     * the warm-up, and the first RECORDED frame would then absorb all of it.
     * That is not hypothetical -- it happened: a 240-frame warm-up produced
     * `ppu_calls/frame=1068` where ~225 is correct (224 + 240*225/64 to the
     * digit), an `apu_lle` skipped-pool sample of 319M cycles, and
     * `ledgerB_resid negative_core_rem_frames=1 FAIL` in both arms of an A/B.
     * Ledger A is safe because it works from per-frame cumulative marks, and
     * Ledger C reads TIM2 deltas -- only these two counters carry state across
     * the boundary, so only these two need clearing. Do it every warm-up frame
     * rather than once at the end: the window's first frame must start from the
     * same zero every other frame does. */
    snes_prof_b_ppu_cyc = snes_prof_b_ppu_calls = 0;
    snes_prof_b_apu_cyc = snes_prof_b_apu_calls = 0;
    snes_prof_b_cpu_cyc = snes_prof_b_cpu_calls = 0;
    snes_prof_b_dma_cyc = snes_prof_b_dma_calls = 0;
    snes_prof_b_spin_cyc = snes_prof_b_spin_calls = 0;
    snes_prof_b_hdma_cyc = snes_prof_b_hdma_calls = 0;
    spin_real_last = g_spin.ops_real;
    spin_virt_last = g_spin.ops_virtual;
    return;
  }

  if (n_drawn + n_skip >= SNES_PROF_FRAMES) {
    snes_profile_dump();
    prof_dumped = true;
    return;
  }

  uint32_t v[BK_COUNT];

  /* Ledger A: consecutive differences of the cumulative marks. Monotonicity is
   * checked rather than assumed -- a mark that went backwards means either
   * something re-cleared CYCCNT underneath us or the frame straddled the
   * ~12.6 s counter wrap, and in both cases every delta in this frame is junk.
   * The frame is still recorded (dropping it would bias the distribution) but
   * the gate counter makes the whole run FAIL. */
  {
    uint32_t prev = active_base;
    for (uint32_t m = 0; m < SNES_PROF_M_COUNT; m++) {
      uint32_t cur = snes_prof_mark[m];
      if ((int32_t)(cur - prev) < 0) g_mark_nonmonotonic++;
      v[BK_FRAMECTL + m] = cur - prev;
      prev = cur;
    }
    v[BK_ACTIVE_TOTAL] = snes_prof_mark[SNES_PROF_M_PACING] - active_base;
  }

  /* Ledger B. core_rem is what is left of the emu OUTER once the two coarse
   * child scopes are removed. It can only go negative if the scopes
   * double-booked (nesting) or a probe leaked, so a negative is a gate failure,
   * not a number to clamp and forget. */
  {
    uint32_t emu_outer = v[BK_EMU];
    uint32_t ppu       = snes_prof_b_ppu_cyc;
    uint32_t apu_emu   = apu_cyc_in_emu;
    v[BK_PPU_INCL] = ppu;
    v[BK_APU_EXCL] = snes_prof_b_apu_cyc;      /* emu-phase + pcm top-up */
    if ((uint64_t)ppu + apu_emu > emu_outer) {
      g_core_rem_negative++;
      v[BK_CORE_REM] = 0;
    } else {
      v[BK_CORE_REM] = emu_outer - ppu - apu_emu;
    }
    /* cpu_only is measured INSIDE core_rem, not beside it: the scope brackets
     * cpu_runOpcode() and subtracts whatever APU/PPU work re-entered through a
     * $2140-3 access, so it never double-books those. core_rem - cpu_only is
     * DMA + the event scheduler + spin bookkeeping. It is clamped rather than
     * left to wrap, and an overrun is a gate failure like any other. */
    v[BK_CPU_EXCL] = (snes_prof_b_cpu_cyc > v[BK_CORE_REM])
                       ? v[BK_CORE_REM] : snes_prof_b_cpu_cyc;
    if (snes_prof_b_cpu_cyc > v[BK_CORE_REM]) g_cpu_over_core_rem++;
    /* dma_only sits inside core_rem too, beside cpu_only. cpu_only already had
     * it subtracted (SNES_PROF_CPU_CALL removes the DMA delta), so
     * core_rem - cpu_only - dma_only is the event scheduler + spin bookkeeping.
     * Clamp to what is left after the interpreter; an overrun is a gate fail. */
    {
      uint32_t rem_after_cpu = v[BK_CORE_REM] - v[BK_CPU_EXCL];
      v[BK_DMA_EXCL] = (snes_prof_b_dma_cyc > rem_after_cpu)
                         ? rem_after_cpu : snes_prof_b_dma_cyc;
      if (snes_prof_b_dma_cyc > rem_after_cpu) g_dma_over_core_rem++;
      /* spin and hdma sit inside core_rem beside cpu/dma; clamp sequentially
       * so the four sub-buckets can never sum past core_rem. Overrun is a
       * gate failure like the others (shared counter: any overrun there
       * already fails the run). */
      uint32_t rem2 = rem_after_cpu - v[BK_DMA_EXCL];
      v[BK_SPIN_EXCL] = (snes_prof_b_spin_cyc > rem2) ? rem2 : snes_prof_b_spin_cyc;
      if (snes_prof_b_spin_cyc > rem2) g_dma_over_core_rem++;
      uint32_t rem3 = rem2 - v[BK_SPIN_EXCL];
      v[BK_HDMA_EXCL] = (snes_prof_b_hdma_cyc > rem3) ? rem3 : snes_prof_b_hdma_cyc;
      if (snes_prof_b_hdma_cyc > rem3) g_dma_over_core_rem++;
    }
    spin_real_sum += g_spin.ops_real - spin_real_last;
    spin_virt_sum += g_spin.ops_virtual - spin_virt_last;
    spin_real_last = g_spin.ops_real;
    spin_virt_last = g_spin.ops_virtual;
    spin_gate_on_at_dump = g_spin.gate_on;
    spin_on_at_dump = g_spin.on;
    call_spin_sum += snes_prof_b_spin_calls;
    call_hdma_sum += snes_prof_b_hdma_calls;
    call_ppu_sum += snes_prof_b_ppu_calls;
    call_apu_sum += snes_prof_b_apu_calls;
    call_cpu_sum += snes_prof_b_cpu_calls;
    call_dma_sum += snes_prof_b_dma_calls;
    if (snes_prof_b_depth != 0) g_depth_nonzero++;
    snes_prof_b_ppu_cyc = snes_prof_b_ppu_calls = 0;
    snes_prof_b_apu_cyc = snes_prof_b_apu_calls = 0;
    snes_prof_b_cpu_cyc = snes_prof_b_cpu_calls = 0;
    snes_prof_b_dma_cyc = snes_prof_b_dma_calls = 0;
    snes_prof_b_spin_cyc = snes_prof_b_spin_calls = 0;
    snes_prof_b_hdma_cyc = snes_prof_b_hdma_calls = 0;
  }

  /* Ledger C + side channels. */
  v[BK_WALL_FRAME]  = wall_frame;
  v[BK_WALL_PACING] = wall_pacing;
  v[BK_DMA_BEFORE]  = dma_before;
  v[BK_DMA_FRAME]   = dma_frame;
  v[BK_WFI]         = wfi_count;
  v[BK_IRQ]         = snes_prof_irq_cycles;
  snes_prof_irq_cycles = 0;
  g_dma_hist[dma_before < 3u ? dma_before : 3u]++;

  {
    uint32_t *pool = drawFrame ? pool_drawn : pool_skip;
    uint64_t *sum  = drawFrame ? sum_drawn  : sum_skip;
    uint32_t  i    = drawFrame ? n_drawn++  : n_skip++;
    for (uint32_t bk = 0; bk < BK_COUNT; bk++) {
      AT(pool, bk, i) = v[bk];
      sum[bk] += v[bk];
    }
  }
}

#endif /* SNES_DEVICE_PROFILE */
