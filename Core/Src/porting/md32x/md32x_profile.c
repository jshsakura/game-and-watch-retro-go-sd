/* Device-side DWT performance profile (MD32X_DEVICE_PROFILE only) — impl.
 *
 * PURPOSE: verify that a QEMU-rig relative win (e.g. Metal Head -51.3%) shows
 * up as an ABSOLUTE fps/cycle improvement on the real STM32H7. QEMU
 * instruction counts are not device cycle counts; only DWT settles the
 * question (see docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md 8).
 *
 * WHY A SEPARATE TU (not inline in main_md32x.c, unlike an earlier version of
 * this feature): the linker script unconditionally pulls main_md32x.o's
 * WHOLE .text/.rodata into the RAM_EMU overlay (STM32H7B0VBTx_SDCARD.ld), and
 * that overlay sits at 99%+ full even before this feature. The profiler's
 * DATA is small (~300B, see the statics below) and AHB-allocated where it
 * isn't, but its CODE -- md32x_profile_dump() alone is a qsort + percentile
 * calculator + a dozen fprintf calls, comfortably 1-2KB of compiled Thumb --
 * is not, and inlining it blew the MD32X BSS ASSERT by 2088B (measured,
 * 0720: reconciling this feature after it was independently built inline on
 * a parallel branch). Keeping it in its own TU routes that code to QSPI
 * flash via the linker script's `.xip_md32x` sweep (`build/md32x/*.o (.text
 * .text*)`, which claims any object not explicitly claimed elsewhere) --
 * only the BSS below (and the tiny md32x_profile_record() call from the
 * main loop) counts against the overlay budget.
 *
 * BYTE-IDENTICAL GUARANTEE: this whole file is only compiled into
 * MD32X_C_SOURCES when MD32X_DEVICE_PROFILE=1 (Makefile), and main_md32x.c's
 * only reference to it (md32x_profile.h's declarations) is itself
 * #ifdef MD32X_DEVICE_PROFILE-gated. The default release build never links
 * or sees any of this.
 *
 * SD-WRITE-ONCE RULE: profiling data is accumulated in RAM and written to
 * /32x_dwt.txt EXACTLY ONCE, after MD32X_PROFILE_FRAMES frames. Reopening
 * the file mid-emulation would distort the very pacing being measured and
 * risks card corruption (the /32x_diag.txt saga, main_md32x.c). A
 * `prof_dumped` flag makes the write strictly one-shot; after it fires,
 * recording stops forever.
 *
 * BUCKET MEANING (disjoint -- the caller in main_md32x.c takes one DWT clear
 * at the top of each loop iteration, then cumulative reads at every phase
 * boundary, and passes them here in order):
 *   pace  : common_emu_frame_loop()              -- frame pacing / wait
 *   proc  : input + pad read + out-buffer setup  -- front-end, before PicoFrame
 *   pico  : PicoFrame()                          -- the emulation (heaviest)
 *   blit  : common_ingame_overlay() + lcd_swap() -- drawn frames only (skip -> ~0)
 *   audio : common_emu_sound_sync(false)         -- audio submit/sync
 *   total : one whole loop iteration             -- over-budget gate uses this
 * The five phase buckets are EXACTLY disjoint (the tama pattern,
 * Core/Src/porting/tama/main_tama.c): `total` is the final cumulative read,
 * so it equals the sum of the five phases plus negligible inter-phase branch
 * cost. No nested or overlapping intervals are ever summed.
 *
 * MEMORY PLACEMENT: the per-frame delta pools CANNOT be static RAM_EMU BSS
 * -- at any usable frame count they would blow the link ASSERT regardless
 * of which TU they live in. They are ahb_calloc'd from the AHB dynamic pool
 * -- which is NOT the raw 120 KB AHBRAM-minus-audio span, it is
 * MD32X_AHB_DYNAMIC_POOL_BYTES (87,904 B, main_md32x.h) after the GBA
 * core's static .gba_ahbram reservation. Draw2FB (main_md32x.c) already
 * takes 83,976 B of that at load, leaving only 3,928 B -- NOT the ~38 KB an
 * earlier version of this comment assumed (that wrong assumption is what
 * let a 28.8 KB request through review and crashed a real device, 0720:
 * "current_ahb_pointer <= __ahbram_audio_start__" assert in
 * gw_malloc.c:ahb_only_malloc). ahb_calloc does NOT fail soft on overflow
 * -- see ahb_get_free_size()'s doc comment in gw_malloc.h -- so the
 * prof_active NULL-fallback below is a real safety net only because
 * MD32X_PROFILE_FRAMES is now sized to actually fit; the _Static_assert
 * next to it is the real guard. Only the tiny accumulators/counters below
 * are static RAM_EMU BSS.
 *
 * PicoFrame() SUB-PHASE BREAKDOWN (rides picodrive's pprof probes): the pico
 * bucket above says how much of the frame is PicoFrame(); it does not say
 * WHICH part dominates -- master SH-2, slave SH-2, 68K, MD VDP draw, 32X
 * compositor draw, or FM/PWM mixing. picodrive already brackets every one of
 * these with pprof_start/pprof_end (external/picodrive/platform/linux/
 * pprof.h and the call sites in pico/32x/32x.c, pico/draw.c,
 * pico/sound/sound.c), normally live only for the QEMU rig's RIG_PHASE_PROF
 * build. pico_int.h auto-defines PPROF (and pprof.h's device timer branch
 * routes pprof_get_one() to md32x_dwt_now() below) whenever
 * MD32X_DEVICE_PROFILE is set, so the exact disjoint accounting the rig uses
 * to rank ROMs now runs on real hardware too -- see pprof.h's disjointness
 * guards (pause/resume around mid-frame draw syncs) for why this is safe to
 * trust: those guards used to be RIG_PHASE_PROF-only, so a device profile
 * run before 0720 was never actually disjoint (draw time double-booked into
 * whichever CPU bucket was open). Fixed at the same time this file was
 * reconciled with the parallel inline version.
 *
 * Storage: pico_int.h declares `pp_counters`/`refcounts` extern; something
 * has to define them once the firmware links picodrive in, same as the QEMU
 * rig does in tools/m7_qemu_rig/rig_32x.c. Sum-only over the whole profiling
 * window (no percentile pools for these) -- the question is which phase
 * dominates PicoFrame() on average, not its frame-to-frame variance.
 */
#include "md32x_profile.h"

#ifdef MD32X_DEVICE_PROFILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <odroid_system.h>
#include "gw_lcd.h"
#include "common.h"
#include "gw_malloc.h"
#include "utils.h"      /* ARRAY_SIZE */

/* Include order MUST match main_md32x.c: pico_types.h before pico.h before
 * pico_int.h. Do NOT add <stdint.h> directly -- it conflicts with m68k.h's
 * `#define uint unsigned int` on this toolchain (pico_types.h pulls it in
 * safely). */
#include "pico/pico_types.h"   /* s8/s16/s32 -- MUST precede pico.h */
#include "pico/pico.h"
#include "pico/pico_int.h"     /* PPROF gate pulls in platform/linux/pprof.h */

#include "main_md32x.h"        /* Pico, PicoIn, MD32X_AUDIO_RATE contract */

/* 0720 device Hardfault: this was 600 (~10 s @60 fps). 600 frames x 6
 * buckets x 2 pools x 4 bytes = 28,800 B of AHB, requested AFTER Draw2FB
 * (83,976 B, main_md32x.c) already ate nearly the whole 87,904 B dynamic
 * pool -- 3,928 B of real headroom, nowhere near 28,800 B. ahb_calloc does
 * not fail soft (see ahb_get_free_size()'s doc comment in gw_malloc.h): it
 * asserts inside ahb_only_malloc, so the prof_active NULL-fallback below
 * was unreachable dead code. Fixed the actual crash by shrinking the
 * window, not by touching Draw2FB (that would change what's being
 * measured) and not by making ahb_calloc fail soft (shared allocator, used
 * by every core -- a separate, carefully-tested change if it happens at
 * all). 64 frames = 3,072 B, leaving 856 B of real margin; the
 * _Static_assert below turns any future regression of either side back
 * into a build failure instead of a device Hardfault. */
#define MD32X_PROFILE_FRAMES  64u   /* ~1.07 s @60 fps / ~1.28 s @50 fps; then dump */
/* Skipped before the window opens. Three device passes measured the SAME
 * 131,537 samples — a boot-anchored window is deterministic and lands on
 * the title sequence, so the numbers profile the logo, not the game.
 * ~20 s in, Doom is in its attract demo (real 3D render) or, if someone
 * is playing, in their gameplay. The phase counters, guest-insn counters
 * and the pcwall probe all start at warmup expiry so every denominator in
 * the dump covers the same window. */
#define MD32X_PROFILE_WARMUP_FRAMES 1200u
#define MD32X_PROF_PATH       "/32x_dwt.txt"

enum {
  PROF_BUCK_PACE = 0,   /* common_emu_frame_loop()                    */
  PROF_BUCK_PROC,       /* input/pad/out-buffer before PicoFrame      */
  PROF_BUCK_PICO,       /* PicoFrame() -- the emulation                */
  PROF_BUCK_BLIT,       /* overlay + lcd_swap (drawn only; skip ~ 0)  */
  PROF_BUCK_AUDIO,      /* common_emu_sound_sync(false)               */
  PROF_BUCK_TOTAL,      /* whole loop iteration                       */
  PROF_BUCK_COUNT
};

/* pcwall block: 2 cores x 64 ROM-page buckets + 2 cores x 3 region sums
 * (sh2pico.c's gnw_pcwall_block_words -- keep in sync). AHB, not overlay
 * BSS: the MD32X overlay BSS sits within a few hundred bytes of
 * __RAM_EMU_END__ and 536 B of static tables broke the link. */
#define MD32X_PCWALL_BLOCK_WORDS (2u * 64u + 2u * 3u)

_Static_assert(2u * PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES * sizeof(uint32_t)
               + MD32X_PCWALL_BLOCK_WORDS * sizeof(uint32_t)
               + MD32X_D2FB_BYTES <= MD32X_AHB_DYNAMIC_POOL_BYTES,
               "32X profiler pools + pcwall block + Draw2FB overflow the AHB "
               "dynamic pool (shrink MD32X_PROFILE_FRAMES) -- see the 0720 "
               "device Hardfault this guards against");

/* Per-frame 32-bit DWT delta pools -- AHB-allocated (see MEMORY PLACEMENT
 * above). Separate drawn/skip pools so render cost does not average into
 * compute-only skips. A per-frame delta at 340 MHz fits easily in 32 bits
 * (60 fps budget is ~5.7M cycles; the raw counter wraps only every ~12.6s). */
static uint32_t *prof_delta_drawn;  /* [PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES] */
static uint32_t *prof_delta_skip;   /* [PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES] */
/* uint64 accumulators: 600 hot pico frames can sum past 2^32, so accumulate
 * the per-frame 32-bit deltas in 64 bits even though each sample is 32-bit. */
static uint64_t prof_sum_drawn[PROF_BUCK_COUNT];
static uint64_t prof_sum_skip[PROF_BUCK_COUNT];
static uint32_t prof_drawn_count;
static uint32_t prof_skip_count;
static bool prof_dumped;    /* strictly one-shot SD write  */
static bool prof_active;    /* false if AHB alloc failed   */
static uint32_t prof_warmup = MD32X_PROFILE_WARMUP_FRAMES;
static unsigned int *pcwall_block;  /* AHB; armed at warmup expiry */

/* sh2pico.c probe contract (also read by the dump below) */
extern void gnw_sh2_pcwall_arm(unsigned int *block);
extern const unsigned int gnw_pcwall_block_words;
/* sh2pico.c opcode-fetch cost probe + interpreter slice counter (both are
 * armed and zeroed by gnw_sh2_pcwall_arm, so they share the same window) */
extern unsigned int gnw_fetch_cyc[2];
extern unsigned int gnw_fetch_n[2];
extern unsigned int gnw_fetch_ovh_x8;
extern unsigned int gnw_sh2_slices[2];
extern unsigned int gnw_da_cyc[2], gnw_da_n[2];    /* guest data reads  */
extern unsigned int gnw_daw_cyc[2], gnw_daw_n[2];  /* guest data writes */

/* flat index into the AHB pools: bucket-major so each bucket's frames are
 * contiguous (qsort in the dump operates on a bucket's slice in place). */
#define PROF_AT(pool, bucket, i)  ((pool)[(bucket) * MD32X_PROFILE_FRAMES + (i)])

static struct pp_counters s_pp_counters;
struct pp_counters *pp_counters = &s_pp_counters;
static int s_pp_refcounts[pp_total_points];
int *refcounts = s_pp_refcounts;

unsigned int md32x_dwt_now(void) { return common_emu_get_dwt_cycles(); }

void md32x_profile_init(void) {
  /* 0720 device Hardfault triage: ahb_only_malloc does NOT return NULL on
   * pool overflow like ram_malloc does -- it advances the bump pointer past
   * __ahbram_audio_start__ and hits gw_malloc.c's assert() (that TU is not
   * built with NDEBUG), which never returns control here at all. The
   * prof_active NULL-check below is therefore unreachable on overflow --
   * this one-shot pre-flight line is the only way to see the real numbers.
   * main_md32x.c's diag_log() is sealed by this point (called right after
   * "entering main loop"), so this appends directly instead. One-shot,
   * boot-adjacent (this function runs exactly once), same allowance as
   * every other boot-time SD write in this file. */
  {
    size_t need = 2 * (size_t)PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES * sizeof(uint32_t);
    size_t free_before = ahb_get_free_size();
    FILE *df = fopen("/32x_diag.txt", "ab");
    if (df) {
      fprintf(df, "profiler init: ahb_free_before=%u need=%u\n",
              (unsigned)free_before, (unsigned)need);
      fclose(df);
    }
  }
  prof_delta_drawn = ahb_calloc((size_t)PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES,
                                sizeof(uint32_t));
  prof_delta_skip  = ahb_calloc((size_t)PROF_BUCK_COUNT * MD32X_PROFILE_FRAMES,
                                sizeof(uint32_t));
  prof_active = (prof_delta_drawn != NULL && prof_delta_skip != NULL);
  if (prof_active) {
    /* Sampled guest-PC wall attribution (sh2pico.c). The block is allocated
     * here but the probe is ARMED at warmup expiry (md32x_profile_record),
     * so its tables cover the same window as everything else; bucket tables
     * live in this AHB block (see the _Static_assert above -- the overlay
     * BSS has no room for them). */
    if (gnw_pcwall_block_words == MD32X_PCWALL_BLOCK_WORDS)
      pcwall_block = ahb_calloc(MD32X_PCWALL_BLOCK_WORDS, sizeof(uint32_t));
  }
  {
    FILE *df = fopen("/32x_diag.txt", "ab");
    if (df) {
      fprintf(df, "profiler init: drawn=%p skip=%p active=%d ahb_free_after=%u\n",
              (void *)prof_delta_drawn, (void *)prof_delta_skip, (int)prof_active,
              (unsigned)ahb_get_free_size());
      fclose(df);
    }
  }
}

static int prof_u32_cmp(const void *a, const void *b) {
  uint32_t ua = *(const uint32_t *)a, ub = *(const uint32_t *)b;
  return (ua > ub) - (ua < ub);
}

/* nearest-rank percentile from an ALREADY SORTED array of n elements (p:0..100) */
static uint32_t prof_pct_sorted(const uint32_t *sorted, uint32_t n, uint32_t p) {
  if (n == 0) return 0;
  uint32_t idx = (n * p) / 100u;
  if (idx >= n) idx = n - 1u;
  return sorted[idx];
}

/* Emit one bucket line to an open file. Sorts the per-frame delta slice IN
 * PLACE -- safe because data is dead after the single dump. kind 0=drawn,1=skip. */
static void prof_emit_bucket(FILE *f, const char *label, uint32_t kind,
                              uint32_t *pool, uint32_t n, uint64_t sum) {
  if (n == 0) {
    fprintf(f, "  %-6s[%c]: n=0\n", label, kind ? 'S' : 'D');
    return;
  }
  qsort(pool, n, sizeof(uint32_t), prof_u32_cmp);
  /* %u, not %lu/%llu: nano.specs' printf here does not parse the 'l'/'ll'
   * length modifier -- it printed the literal characters "lu" on device
   * (0720 /32x_dwt.txt). Values fit unsigned int at this window size
   * (MD32X_PROFILE_FRAMES=64, budget ~5.7M cycles/frame -- nowhere near
   * 2^32) so a plain cast is exact, not truncating. */
  fprintf(f, "  %-6s[%c]: n=%u avg=%u p50=%u p90=%u p95=%u p99=%u\n",
          label, kind ? 'S' : 'D', (unsigned)n,
          (unsigned)(sum / n),
          (unsigned)prof_pct_sorted(pool, n, 50),
          (unsigned)prof_pct_sorted(pool, n, 90),
          (unsigned)prof_pct_sorted(pool, n, 95),
          (unsigned)prof_pct_sorted(pool, n, 99));
}

/* One controlled dump to /32x_dwt.txt: mute audio, open/write/close once,
 * refresh the watchdog around each slow step, unmute. Never called more than
 * once (guarded by prof_dumped at the call site, md32x_profile_record). */
static void md32x_profile_dump(void) {
  extern uint32_t SystemCoreClock;   /* system_stm32h7xx.c */

  /* Budget: cycles available per frame at the current clock and region.
   *   frame_time_10us is in 10-us ticks; SystemCoreClock/100000 converts a
   *   10-us tick to cycles (10 us = 1e-4 s; clk * 1e-4 = clk/100000). */
  uint64_t budget = (uint64_t)common_emu_state.frame_time_10us
                    * (uint64_t)(SystemCoreClock / 100000u);

  /* Over-budget frames (loop_total > budget) for drawn and skip separately. */
  uint32_t over_drawn = 0, over_skip = 0;
  uint32_t *total_drawn = prof_delta_drawn + PROF_BUCK_TOTAL * MD32X_PROFILE_FRAMES;
  uint32_t *total_skip  = prof_delta_skip  + PROF_BUCK_TOTAL * MD32X_PROFILE_FRAMES;
  for (uint32_t i = 0; i < prof_drawn_count; i++)
    if (total_drawn[i] > budget) over_drawn++;
  for (uint32_t i = 0; i < prof_skip_count; i++)
    if (total_skip[i] > budget) over_skip++;

  /* Mute so the dump does not buzz; harmless if already muted. */
  odroid_audio_mute(true);
  wdog_refresh();

  FILE *f = fopen(MD32X_PROF_PATH, "wb");
  wdog_refresh();
  if (f == NULL) { odroid_audio_mute(false); return; }

  fprintf(f, "=== 32X DWT device profile ===\n");
  fprintf(f, "build: MD32X_DEVICE_PROFILE=1 (opt switch ON)\n");
  fprintf(f, "window: opened after %u warmup frames (~%u s) -- all counters "
          "zeroed at open\n",
          (unsigned)MD32X_PROFILE_WARMUP_FRAMES,
          (unsigned)(MD32X_PROFILE_WARMUP_FRAMES / 60u));
  /* No firmware-commit symbol is exported by this build (no _build_version /
   * git-rev global); the file path + opt-switch line identify the run. */
  fprintf(f, "clk=%u Hz  region=%s  oc_user=%u (common_emu_auto_oc floor=1)\n",
          (unsigned)SystemCoreClock,
          Pico.m.pal ? "PAL" : "NTSC",
          (unsigned)odroid_settings_cpu_oc_level_get());
  fprintf(f, "frame_budget=%u cycles  frames_drawn=%u frames_skip=%u  total=%u\n",
          (unsigned)budget,
          (unsigned)prof_drawn_count, (unsigned)prof_skip_count,
          (unsigned)(prof_drawn_count + prof_skip_count));
  fprintf(f, "over_budget_total(drawn)=%u/%u  over_budget_total(skip)=%u/%u\n",
          (unsigned)over_drawn, (unsigned)prof_drawn_count,
          (unsigned)over_skip, (unsigned)prof_skip_count);
  fprintf(f, "buckets (D=drawn, S=skip; values are DWT cycles):\n");
  wdog_refresh();

  static const char *const names[PROF_BUCK_COUNT] = {
    "pace", "proc", "pico", "blit", "audio", "total"
  };
  for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
    prof_emit_bucket(f, names[b], 0,
                     prof_delta_drawn + b * MD32X_PROFILE_FRAMES,
                     prof_drawn_count, prof_sum_drawn[b]);
    prof_emit_bucket(f, names[b], 1,
                     prof_delta_skip + b * MD32X_PROFILE_FRAMES,
                     prof_skip_count, prof_sum_skip[b]);
  }

  /* PicoFrame() sub-phase breakdown (picodrive pprof probes, sum-only --
   * see the block comment at the top of this file). pico_total is the SAME
   * quantity PROF_BUCK_PICO measured from the outside; printed so the two
   * can be cross-checked. Any nonzero refcount below means a
   * pprof_start/pprof_end pair leaked (should never happen -- see the
   * widened disjointness guards in draw.c/32x.c/sound.c) and the sums above
   * it are suspect. Also: sub-bucket percentages here can legitimately sum
   * to MORE than 100% of pico_total if a leak or a missing pause/resume
   * guard lets two buckets double-book the same cycles -- refcount_leaks=0
   * is the thing to check before trusting any of the percentages. */
  {
    uint32_t total_frames = prof_drawn_count + prof_skip_count;
    uint64_t pico_total = prof_sum_drawn[PROF_BUCK_PICO] + prof_sum_skip[PROF_BUCK_PICO];
    static const struct { int point; const char *label; } phases[] = {
      { pp_frame,   "frame"   },  /* whole PicoFrame() -- cross-check vs pico above */
      { pp_msh2,    "msh2"    },  /* master SH-2 interpreter                       */
      { pp_ssh2,    "ssh2"    },  /* slave SH-2 interpreter                        */
      { pp_m68k,    "m68k"    },  /* 68000 interpreter                            */
      { pp_draw,    "draw_md" },  /* MD VDP line render (pico/draw.c)             */
      { pp_draw32x, "draw32x" },  /* 32X compositor layer merge (pico/32x/draw.c) */
      { pp_sound,   "sound"   },  /* PSG/mix, excludes fm/pwm paused sub-windows   */
      { pp_fm,      "fm"      },  /* YM2612 render                                */
      { pp_pwm,     "pwm"     },  /* 32X PWM chip render                          */
    };
    int leaked = 0;
    for (uint32_t i = 0; i < ARRAY_SIZE(phases); i++)
      if (s_pp_refcounts[phases[i].point] != 0) leaked++;

    fprintf(f, "picoframe sub-phases (sum over whole window, refcount_leaks=%d):\n",
            leaked);
    fprintf(f, "  pico_total(outside)=%u  frame_total(pprof)=%u\n",
            (unsigned)pico_total,
            (unsigned)s_pp_counters.counter[pp_frame]);
    for (uint32_t i = 0; i < ARRAY_SIZE(phases); i++) {
      uint64_t sum = (uint64_t)s_pp_counters.counter[phases[i].point];
      uint64_t avg = total_frames ? sum / total_frames : 0;
      unsigned pct_x10 = pico_total ? (unsigned)((sum * 1000) / pico_total) : 0;
      fprintf(f, "  %-8s: sum=%u avg/frame=%u pct_of_pico=%u.%u%%\n",
              phases[i].label, (unsigned)sum,
              (unsigned)avg, pct_x10 / 10, pct_x10 % 10);
    }

    /* cycles-per-guest-instruction: is msh2/ssh2 cost dispatch overhead
     * (both cores' ratio close together) or memory-stall-bound (one core's
     * ratio far higher -- e.g. it executes more game code straight out of
     * XIP flash than the other)? The QEMU rig can only report this ratio
     * against host INSTRUCTIONS (no cache model); this is the same ratio
     * against real DWT CYCLES, the number the rig cannot produce. */
    extern unsigned long long gnw_sh2_insn_count[2];
    uint64_t msh2_cyc = (uint64_t)s_pp_counters.counter[pp_msh2];
    uint64_t ssh2_cyc = (uint64_t)s_pp_counters.counter[pp_ssh2];
    uint64_t msh2_insn = gnw_sh2_insn_count[0];
    uint64_t ssh2_insn = gnw_sh2_insn_count[1];
    fprintf(f, "sh2 cycles/guest-insn (device, DWT cycles / dispatched guest insns):\n");
    fprintf(f, "  msh2: cyc=%u insn=%u ratio=%u.%u\n",
            (unsigned)msh2_cyc, (unsigned)msh2_insn,
            msh2_insn ? (unsigned)(msh2_cyc / msh2_insn) : 0u,
            msh2_insn ? (unsigned)((msh2_cyc * 10 / msh2_insn) % 10) : 0u);
    fprintf(f, "  ssh2: cyc=%u insn=%u ratio=%u.%u\n",
            (unsigned)ssh2_cyc, (unsigned)ssh2_insn,
            ssh2_insn ? (unsigned)(ssh2_cyc / ssh2_insn) : 0u,
            ssh2_insn ? (unsigned)((ssh2_cyc * 10 / ssh2_insn) % 10) : 0u);
  }

  /* Sampled guest-PC wall attribution (sh2pico.c probe). Freezes the probe
   * first so no tick mutates the tables mid-read (single-threaded anyway;
   * the freeze is what stops post-dump sampling cost). Shares are of the
   * core's OWN attributed total — the pct_of_pico lines above scale them
   * to the frame. rom<64K rows are 1 KB pages, top-12 by cycles. */
  {
    enum { PCW_NBUCK = 64, PCW_ROM_HI = 0, PCW_SDRAM = 1, PCW_OTHER = 2 };
    extern int gnw_pcwall_armed;
    extern unsigned int *gnw_pcwall_hist_p[2];
    extern unsigned int *gnw_pcwall_region_p[2];
    extern unsigned int gnw_pcwall_samples[2];
    extern const unsigned int gnw_pcwall_win_base;
    extern const unsigned int gnw_pcwall_page_shift;
    static const char *const core_names[2] = { "msh2", "ssh2" };
    unsigned int page_kb = (1u << gnw_pcwall_page_shift) >> 10;

    gnw_pcwall_armed = 0;
    fprintf(f, "sh2 guest-PC wall attribution (sampled every 32 insns, DWT cycles):\n");
    fprintf(f, "  window: rom+0x%x, 64 pages x %u KB\n",
            gnw_pcwall_win_base, page_kb);
    for (int core = 0; gnw_pcwall_hist_p[0] != NULL && core < 2; core++) {
      const unsigned int *hist = gnw_pcwall_hist_p[core];
      const unsigned int *region = gnw_pcwall_region_p[core];
      uint64_t win = 0, total;
      for (int i = 0; i < PCW_NBUCK; i++) win += hist[i];
      total = win + region[PCW_ROM_HI] + region[PCW_SDRAM] + region[PCW_OTHER];
      fprintf(f, "  %s: samples=%u attributed=%u cycles\n", core_names[core],
              gnw_pcwall_samples[core], (unsigned)total);
      if (total == 0) continue;
      fprintf(f, "    rom_win=%u.%u%% rom_hi=%u.%u%% sdram=%u.%u%% other=%u.%u%%\n",
              (unsigned)(win * 1000 / total) / 10, (unsigned)(win * 1000 / total) % 10,
              (unsigned)((uint64_t)region[PCW_ROM_HI] * 1000 / total) / 10,
              (unsigned)((uint64_t)region[PCW_ROM_HI] * 1000 / total) % 10,
              (unsigned)((uint64_t)region[PCW_SDRAM] * 1000 / total) / 10,
              (unsigned)((uint64_t)region[PCW_SDRAM] * 1000 / total) % 10,
              (unsigned)((uint64_t)region[PCW_OTHER] * 1000 / total) / 10,
              (unsigned)((uint64_t)region[PCW_OTHER] * 1000 / total) % 10);
      /* top-12 window pages by selection (64 entries; no qsort scratch) */
      uint8_t used[PCW_NBUCK] = { 0 };
      for (int rank = 0; rank < 12; rank++) {
        int best = -1;
        for (int i = 0; i < PCW_NBUCK; i++)
          if (!used[i] && hist[i] &&
              (best < 0 || hist[i] > hist[best]))
            best = i;
        if (best < 0) break;
        used[best] = 1;
        unsigned pct_x10 = (unsigned)((uint64_t)hist[best] * 1000 / total);
        fprintf(f, "    rom page 0x%08x: cyc=%u (%u.%u%%)\n",
                0x02000000u + gnw_pcwall_win_base
                    + ((unsigned)best << gnw_pcwall_page_shift),
                hist[best], pct_x10 / 10, pct_x10 % 10);
      }
      wdog_refresh();
    }
  }

  /* Opcode-fetch cost (sh2pico.c probe) and interpreter slice granularity.
   *
   * avg is the raw bracketed delta; net subtracts the measured CYCCNT-pair
   * self-cost, so net is what one fetch really costs.  est_pct extrapolates
   * net over ALL fetches (one per dispatched instruction) as a share of the
   * core's window cycles: that is the fraction of the 100-135 cycles/insn
   * that is memory, and it decides the next lever — single digits means the
   * fetch is cached and dispatch is the cost; tens of percent means flash
   * latency and the ROM wants caching in RAM.
   *
   * insn/slice is the control: ~1300 is the designed STEP_N granularity, a
   * small number would mean poll-forced early syncs and per-slice overhead. */
  {
    extern unsigned long long gnw_sh2_insn_count[2];
    uint64_t sh2_win[2];
    sh2_win[0] = (uint64_t)s_pp_counters.counter[pp_msh2];
    sh2_win[1] = (uint64_t)s_pp_counters.counter[pp_ssh2];
    fprintf(f, "sh2 opcode-fetch cost (1 in 31 direct fetches bracketed; "
               "CYCCNT-pair self-cost %u.%u cyc):\n",
            gnw_fetch_ovh_x8 / 8u, ((gnw_fetch_ovh_x8 * 10u) / 8u) % 10u);
    for (int core = 0; core < 2; core++) {
      uint32_t n = gnw_fetch_n[core];
      uint32_t avg_x10 = n ? (uint32_t)(((uint64_t)gnw_fetch_cyc[core] * 10) / n) : 0;
      uint32_t ovh_x10 = (gnw_fetch_ovh_x8 * 10u) / 8u;
      uint32_t net_x10 = avg_x10 > ovh_x10 ? avg_x10 - ovh_x10 : 0;
      uint64_t insn = gnw_sh2_insn_count[core];
      uint32_t est_pct_x10 = sh2_win[core]
        ? (uint32_t)((insn * net_x10 * 100) / sh2_win[core]) : 0;
      fprintf(f, "  %s: n=%u avg=%u.%u net=%u.%u est_fetch_share=%u.%u%%"
                 " slices=%u insn/slice=%u\n",
              core ? "ssh2" : "msh2", n,
              avg_x10 / 10, avg_x10 % 10, net_x10 / 10, net_x10 % 10,
              est_pct_x10 / 10, est_pct_x10 % 10,
              gnw_sh2_slices[core],
              gnw_sh2_slices[core] ? (unsigned)(insn / gnw_sh2_slices[core]) : 0u);
      wdog_refresh();
    }

    /* Guest DATA accesses, same bracketing. share is net*count extrapolated
     * over ALL accesses (the probe samples 1 in 29), so fetch_share +
     * read_share + write_share is the memory fraction of the core's wall and
     * whatever is left is the interpreter's own decode/execute. per_insn says
     * how many loads/stores a dispatched instruction actually performs. */
    fprintf(f, "sh2 data-access cost (1 in 29 bracketed; the probe's own call "
               "is outside the bracket):\n");
    for (int core = 0; core < 2; core++) {
      uint32_t ovh_x10 = (gnw_fetch_ovh_x8 * 10u) / 8u;
      uint64_t insn = gnw_sh2_insn_count[core];
      struct { const char *name; uint32_t n, cyc; } cls[2] = {
        { "read",  gnw_da_n[core],  gnw_da_cyc[core]  },
        { "write", gnw_daw_n[core], gnw_daw_cyc[core] },
      };
      fprintf(f, "  %s:", core ? "ssh2" : "msh2");
      for (int k = 0; k < 2; k++) {
        uint32_t n = cls[k].n;
        uint32_t avg_x10 = n ? (uint32_t)(((uint64_t)cls[k].cyc * 10) / n) : 0;
        uint32_t net_x10 = avg_x10 > ovh_x10 ? avg_x10 - ovh_x10 : 0;
        /* total accesses ~= n * period; share = total * net / window */
        uint64_t total = (uint64_t)n * 29u;
        uint32_t share_x10 = sh2_win[core]
          ? (uint32_t)((total * net_x10 * 100) / sh2_win[core]) : 0;
        uint32_t per_insn_x100 = insn ? (uint32_t)((total * 100) / insn) : 0;
        fprintf(f, " %s n=%u net=%u.%u share=%u.%u%% per_insn=%u.%02u",
                cls[k].name, n, net_x10 / 10, net_x10 % 10,
                share_x10 / 10, share_x10 % 10,
                per_insn_x100 / 100, per_insn_x100 % 100);
      }
      fprintf(f, "\n");
      wdog_refresh();
    }
  }

  wdog_refresh();
  fclose(f);
  wdog_refresh();
  odroid_audio_mute(false);
}

void md32x_profile_record(bool drawFrame, uint32_t t_pace, uint32_t t_proc,
                           uint32_t t_pico, uint32_t t_blit, uint32_t t_audio) {
  if (!prof_active || prof_dumped) return;

  /* Warmup: discard the boot/title frames, then open the window with every
   * counter zeroed at the same instant. record() runs between frames, so
   * no pprof pair is open (refcounts all zero) and the resets are clean. */
  if (prof_warmup) {
    if (--prof_warmup == 0) {
      extern unsigned long long gnw_sh2_insn_count[2];
      memset(&s_pp_counters, 0, sizeof(s_pp_counters));
      gnw_sh2_insn_count[0] = gnw_sh2_insn_count[1] = 0;
      /* arms AND zeroes the fetch probe + slice counters (sh2pico.c) */
      gnw_sh2_pcwall_arm(pcwall_block);   /* NULL leaves the probe disarmed */
    }
    return;
  }

  uint32_t total = prof_drawn_count + prof_skip_count;
  if (total >= MD32X_PROFILE_FRAMES) {
    /* Window full: one controlled SD dump (mutes audio, kicks wdog
     * internally), then never record or write again. */
    md32x_profile_dump();
    prof_dumped = true;
    return;
  }

  uint32_t delta[PROF_BUCK_COUNT];
  delta[PROF_BUCK_PACE]  = t_pace;
  delta[PROF_BUCK_PROC]  = t_proc - t_pace;
  delta[PROF_BUCK_PICO]  = t_pico - t_proc;
  delta[PROF_BUCK_BLIT]  = t_blit - t_pico;   /* ~0 on skip: nothing renders */
  delta[PROF_BUCK_AUDIO] = t_audio - t_blit;
  delta[PROF_BUCK_TOTAL] = t_audio;           /* since the single clear */

  if (drawFrame) {
    uint32_t i = prof_drawn_count++;
    for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
      PROF_AT(prof_delta_drawn, b, i) = delta[b];
      prof_sum_drawn[b] += delta[b];
    }
  } else {
    uint32_t i = prof_skip_count++;
    for (uint32_t b = 0; b < PROF_BUCK_COUNT; b++) {
      PROF_AT(prof_delta_skip, b, i) = delta[b];
      prof_sum_skip[b] += delta[b];
    }
  }
}

#endif /* MD32X_DEVICE_PROFILE */
