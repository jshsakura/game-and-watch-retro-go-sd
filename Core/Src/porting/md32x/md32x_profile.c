/* Device-side DWT phase profiler for 32X (picodrive) — implementation.
 *
 * Lives in its own translation unit (NOT main_md32x.c) so the linker script's
 * .xip_md32x sweep (`build/md32x/*.o (.text .text*)`) routes this file's code
 * to QSPI flash instead of the RAM_EMU overlay. main_md32x.o's .text is
 * explicitly pulled into overlay RAM by STM32H7B0VBTx_SDCARD.ld, so any heavy
 * code added there eats the overlay budget. Keeping the profile's
 * snprintf/fopen/fwrite code here puts the cost in flash; only the small
 * pp_counters+refcounts arrays stay in overlay (each pp_total_points entry:
 * sizeof(pp_type)=8B + sizeof(int)=4B). Budget history: 188B RAM_EMU
 * headroom when this file was written (0719) meant pp_fm/pp_pwm/pp_draw32x
 * had to stay RIG_PHASE_PROF-only; the SH-2 interpreter's move into ITCM
 * (32X_SH2_ITCM_ANALYSIS.md, 0720) freed enough that pp_draw32x now fits
 * (see md32x_run_profile's bucket table) — re-check gnw_hw_harness before
 * adding pp_fm/pp_pwm too, don't assume the same headroom still holds by
 * the time you read this.
 *
 * The pprof probes themselves live in picodrive (pico/pico.c, draw.c,
 * 32x/32x.c, sound/sound.c) and are activated by -DPPROF -DGNW_32X_DWT_PROF
 * (see Makefile). pprof_get_one() calls our md32x_dwt_now() which reads
 * DWT_CYCCNT — set up by common_emu_enable_dwt_cycles().
 *
 * Output (/32x_dwt.txt) answers: is slave SH-2 interpret (pp_ssh2), MD VDP
 * line render (pp_draw), or 32X layer compositing (pp_draw32x) the
 * frame-time max consumer? That decides the next lever (interpreter limit,
 * or a DMA2D opportunity — and which of the two draw buckets it would even
 * apply to, since only pp_draw's plain CLUT8->RGB565 pass is a format-
 * conversion DMA2D can do at all; pp_draw32x's per-pixel layer selection
 * is not, see 32X_DRAW_PATH_ANALYSIS.md). Buckets are disjoint (0720: fixed
 * pprof.h's pause/resume guards, previously RIG_PHASE_PROF-only, so device
 * profiles were double-booking mid-frame draw-sync time into whichever CPU
 * bucket was open) — they should sum close to 100% of pp_frame, not over. */
#include <stdio.h>
#include <string.h>

#include "gw_lcd.h"            /* lcd_get_active_buffer */
#include "common.h"            /* common_emu_*_dwt_cycles, wdog_refresh */

/* Include order MUST match main_md32x.c: pico_types.h before pico.h before
 * pico_int.h. Do NOT add <stdint.h> directly — it conflicts with m68k.h's
 * `#define uint unsigned int` on this toolchain (pico_types.h pulls it in
 * safely). */
#include "pico/pico_types.h"   /* s8/s16/s32 — MUST precede pico.h */
#include "pico/pico.h"
#include "pico/pico_int.h"     /* PPROF gate pulls in platform/linux/pprof.h */

#include "main_md32x.h"        /* Pico, PicoIn, app_main_md32x contract */

#ifdef MD32X_DEVICE_PROFILE

/* pprof.h declares these as externs; platform/linux/pprof.c (which provides
 * them upstream) is NOT in the device source list. Define them here. They are
 * non-static so the picodrive object files referencing them resolve at link
 * time. */
static struct pp_counters md32x_pp_counters;
static int md32x_refcounts[pp_total_points];
struct pp_counters *pp_counters = &md32x_pp_counters;
int *refcounts = md32x_refcounts;

/* DWT_CYCCNT read for pprof_get_one() (platform/linux/pprof.h calls this under
 * the MD32X_DEVICE_PROFILE branch). 32-bit, wraps at ~10 s @ 400 MHz —
 * per-frame deltas are tiny and unsigned arithmetic handles wrap correctly.
 * Return type must be plain `unsigned int`, matching pprof.h's extern decl
 * exactly -- `uint32_t` is `unsigned long` on this ABI (arm-none-eabi
 * ILP32: same width, distinct type), a conflicting-types build error even
 * though the two are bit-identical. */
unsigned int md32x_dwt_now(void) {
  return common_emu_get_dwt_cycles();
}

/* pprof.h declares these; provide no-ops (storage is static above). */
void pprof_init(void)  { memset(&md32x_pp_counters, 0, sizeof(md32x_pp_counters));
                          memset(md32x_refcounts, 0, sizeof(md32x_refcounts)); }
void pprof_finish(void) { /* no-op — report happens in md32x_run_profile */ }

/* Inlined copy of main_md32x.c's static set_out_buffer() so this file is
 * self-contained. PicoDrawSetOutBuf must be re-pointed each frame to the
 * active LCD buffer. */
static void md32x_set_out_buffer(void) {
  PicoDrawSetOutBuf(lcd_get_active_buffer(), 320 * 2);
}

void md32x_run_profile(void) {
  extern uint32_t SystemCoreClock;
  common_emu_enable_dwt_cycles();
  memset(&md32x_pp_counters, 0, sizeof(md32x_pp_counters));
  memset(md32x_refcounts, 0, sizeof(md32x_refcounts));

  PicoIn.pad[0] = 0;
  PicoIn.skipFrame = 0;
  const int N = 120;
  uint32_t frame_max = 0;          /* worst single-frame DWT delta */
  uint32_t frame_first = 0;        /* first-frame cost (cold caches) */
  for (int i = 0; i < N; i++) {
    wdog_refresh();
    common_emu_clear_dwt_cycles();
    md32x_set_out_buffer();
    PicoFrame();
    uint32_t fc = common_emu_get_dwt_cycles();
    if (i == 0) frame_first = fc;
    if (fc > frame_max) frame_max = fc;
  }

  /* Per-bucket averages + % of pp_frame. pp_frame wraps the whole PicoFrame
   * body; sub-buckets are non-overlapping phases that should sum to ~pp_frame
   * -- true disjointness (not just non-overlapping call sites) required
   * extending pprof.h's pause/resume guards from RIG_PHASE_PROF-only to also
   * cover GNW_32X_DWT_PROF (0720): mid-frame draw sync fires from CPU memory
   * handlers, so without pausing the live CPU accumulator during it, draw
   * time double-booked into whichever of msh2/ssh2/m68k was open AND into
   * draw/draw32x -- the buckets would not have actually summed to ~100%.
   * Naming the buckets via a table keeps the report self-describing on the SD
   * card. pp_fm/pp_pwm stay RIG_PHASE_PROF-only (not needed for the
   * draw-vs-composite question below, and no need to spend more of the
   * RAM_EMU budget than this decision requires). */
  static const struct { int idx; const char *name; const char *desc; } buckets[] = {
    { pp_frame,   "frame",   "PicoFrame total (outer)"                      },
    { pp_msh2,    "msh2",    "master SH-2 interpret"                        },
    { pp_ssh2,    "ssh2",    "slave SH-2 interpret"                         },
    { pp_m68k,    "m68k",    "68000 interpret (gwenesis)"                   },
    { pp_z80,     "z80",     "Z80 interpret (cz80)"                         },
    { pp_draw,    "draw",    "MD VDP line render (pico/draw.c)"             },
    { pp_draw32x, "draw32x", "32X compositor (pico/32x/draw.c layer merge)" },
    { pp_sound,   "sound",   "sound render total"                           },
  };
  const int nbuckets = sizeof(buckets) / sizeof(buckets[0]);
  pp_type frame_tot = md32x_pp_counters.counter[pp_frame];
  pp_type frame_avg = frame_tot / (pp_type)N;

  enum { RBUF = 1536 };
  char rbuf[RBUF];
  int off = snprintf(rbuf, RBUF,
      "32X DWT profile (MD32X_DEVICE_PROFILE)\n"
      "frames=%d  clk=%lu MHz  DWT_CYCCNT @ 1 cyc/insn-class\n"
      "frame_total:   %lu cyc/frame avg  (%lu.%lu ms @ clk)\n"
      "frame_first:   %lu cyc  frame_max: %lu cyc\n",
      N, (unsigned long)SystemCoreClock / 1000000u,
      (unsigned long)frame_avg,
      (unsigned long)(frame_avg / (SystemCoreClock / 1000u)),
      (unsigned long)((frame_avg % (SystemCoreClock / 1000u))
                      / ((SystemCoreClock / 1000u) / 10u)),
      (unsigned long)frame_first, (unsigned long)frame_max);
  unsigned pct_sum = 0;   /* sub-buckets only (skip buckets[0] == pp_frame, the denominator) */
  for (int i = 0; i < nbuckets && off < RBUF - 80; i++) {
    pp_type v = md32x_pp_counters.counter[buckets[i].idx];
    pp_type avg = v / (pp_type)N;
    unsigned pct = frame_avg ? (unsigned)(v * 100u / frame_tot) : 0u;
    if (i > 0) pct_sum += pct;
    off += snprintf(rbuf + off, RBUF - off, "%-10s %8lu cyc/f  %3u%%  %s\n",
                    buckets[i].name, (unsigned long)avg, pct, buckets[i].desc);
  }
  /* Disjointness self-check: sub-buckets should sum close to 100% of
   * pp_frame, not meaningfully over. A sum well past 100% means two buckets
   * are double-booking the same cycles (a pause/resume guard missing
   * somewhere) -- don't trust the individual percentages above until fixed.
   * Some slack is expected: pp_frame includes scheduler/event/timer/memory
   * glue no sub-bucket claims, so sub-buckets summing UNDER 100% is normal. */
  off += snprintf(rbuf + off, RBUF - off,
      "sub-bucket sum: %u%% of frame (>100%% = buckets overlap, don't trust the split above)\n",
      pct_sum);
  FILE *pf = fopen("/32x_dwt.txt", "wb");
  if (pf) { fwrite(rbuf, 1, (size_t)off, pf); fclose(pf); }
  /* No diag_log breadcrumb: keeping diag_log static in main_md32x.c preserves
   * byte-identical default builds (a global diag_log shifts the link graph). */
}

#endif /* MD32X_DEVICE_PROFILE */
