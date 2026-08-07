/* Pacing-block integration test for the SNES audio path.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The stretcher unit test (test_snes_audio_stretch.c) drives push/pull
 * directly and missed the pacing bug: the block in main_snes.c that ORCHESTRATES
 * push and emit was never compiled by any test. A regression shipped, was caught
 * only on device ("completely different sound"), and the root cause was the
 * catch-up loop calling snes_pcm_emit() two or three times inside one DMA
 * period — each emit pulls a full buffer from the stretcher ring, so the ring
 * drained two to three times faster than the DMA consumed it, ran permanently
 * dry, and what played was the stretcher's dropout filler rather than the game.
 *
 * This test compiles and runs the ACTUAL pacing block extracted from
 * main_snes.c (see tests/run.sh: the shell writes lines from the `if
 * (speedupEnabled == SPEEDUP_1x)` marker through the line before `Catch-up
 * only for HLE` into snes_pacing_block.inc, and this file #includes it). A
 * reimplementation would test a different program — the disease this repo
 * caught three times (hw_jpeg_decoder.c, sm_harness, the stretcher test
 * itself).
 *
 * WHAT IT CHECKS
 * --------------
 * One invariant, hard: at most ONE snes_pcm_emit() per DMA period. Two emits
 * inside the same period both write to the same audio_get_active_buffer()
 * return value (the half-buffer the DMA just freed), so the second overwrites
 * the first, and both pull from the stretcher ring — draining it at a multiple
 * of the real consumption rate. The device hears that as the stretcher running
 * dry and replaying history: "완전히 다른 소리".
 *
 * Second invariant, softer: the stretcher ring must not run dry in steady
 * state at any fps. The pacing fix's contract is "a slow frame leaves the
 * period it missed playing stale audio" — NOT "the ring goes dry and filler
 * takes over". Underruns > 0 after warm-up means the pacing is pulling more
 * than the core pushed.
 *
 * DMA MODEL
 * ---------
 * One DMA period = 266 samples @ 16 kHz = 16.625 ms. dma_counter is
 * incremented at each period boundary (the device does it in the SAI half/full
 * ISR; the test increments it directly). cpumon_sleep() on device is __WFI()
 * until that ISR fires — the mock advances dma_counter by one and flips the
 * active half-buffer, which is exactly what the ISR does. A frame whose
 * emulation took N periods finds dma_counter already advanced by N at the
 * pacing-block entry; a frame faster than one period finds it unchanged and
 * the initial wait loop calls cpumon_sleep() once to synchronise, exactly as
 * on hardware.
 *
 * RED / GREEN
 * -----------
 * The test runner extracts the pacing block twice:
 *   RED:   from HEAD (git show HEAD:Core/Src/porting/snes/main_snes.c)
 *   GREEN: from the working tree (the file as it stands now)
 * HEAD has the catch-up while(elapsed>1){emit();...} loop; the working tree
 * has the (void)elapsed; fix. RED MUST fail the invariant. GREEN MUST pass.
 * A test that has never been red proves nothing — see CLAUDE.md.
 */

/* HEADLESS must be defined before the pacing block sees any device-only
 * headers. The block itself only references dma_counter, wdog_refresh,
 * cpumon_sleep, snes_pcm_emit, and odroid_system_get_app — all defined as
 * mocks below. No device headers are included. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../Core/Src/porting/snes/snes_audio_stretch.h"

/* ---- mocks for everything the pacing block references --------------------- */

#define SNES_FPS          60
#define SNES_AUDIO_RATE   16000
#define SNES_AUDIO_SAMPLES (SNES_AUDIO_RATE / SNES_FPS)   /* 266 */
#define DMA_PERIOD_SAMPLES SNES_AUDIO_SAMPLES             /* 266, one half-buf */
#define PERIOD_MS         (1000.0 * DMA_PERIOD_SAMPLES / SNES_AUDIO_RATE)  /* 16.625 */

#define SPEEDUP_1x 0

typedef struct { int speedupEnabled; } odroid_app_t;
static odroid_app_t g_app = { .speedupEnabled = SPEEDUP_1x };
odroid_app_t *odroid_system_get_app(void) { return &g_app; }

/* The DMA tick counter. Volatile because the pacing block's initial wait
 * loop re-reads it every iteration — without volatile a host compiler may
 * keep it in a register and the loop never exits, which is exactly the
 * same reason gw_audio.h marks it volatile. */
volatile uint32_t dma_counter = 0;

/* Active half-buffer selector: flips on every DMA tick. Two emits at the same
 * dma_counter return the same pointer — that is the overwrite the bug causes. */
static int16_t g_buf_a[DMA_PERIOD_SAMPLES];
static int16_t g_buf_b[DMA_PERIOD_SAMPLES];
static int     g_which_buf = 0;

int16_t *audio_get_active_buffer(void) {
    return g_which_buf ? g_buf_b : g_buf_a;
}
uint16_t audio_get_buffer_length(void) { return DMA_PERIOD_SAMPLES; }

bool     common_emu_sound_loop_is_muted(void) { return false; }
uint8_t  common_emu_sound_get_volume(void)    { return 255; }   /* unity */

unsigned g_wdog_feeds = 0;
void wdog_refresh(void) { g_wdog_feeds++; }

/* cpumon_sleep on device = __WFI() until the DMA ISR fires. The ISR increments
 * dma_counter and flips the active buffer. The mock does the same. */
void cpumon_sleep(void) {
    dma_counter++;
    g_which_buf ^= 1;
}

/* ---- emit / submit (test versions using the REAL stretcher) -------------- */

/* The invariant tracker. last_emit_dma records the dma_counter at the most
 * recent emit. A second emit at the same value is the bug: same half-buffer,
 * stretcher ring drained twice for one period. */
static uint32_t g_last_emit_dma = 0xFFFFFFFF;
static unsigned g_double_emit_periods = 0;   /* periods where ≥2 emits landed */
static unsigned g_emits_total = 0;
static unsigned g_max_fill_seen = 0;

/* Output inspection: record every sample the DMA would have played, so a
 * post-run scan can detect long runs of identical samples (filler signature)
 * even when they are non-zero (the stretcher plays history, not silence). */
static int16_t  g_played[600 * DMA_PERIOD_SAMPLES];  /* ~600 periods */
static unsigned g_played_count = 0;

static void record_output(const int16_t *buf, uint16_t n) {
    for (uint16_t i = 0; i < n && g_played_count < sizeof(g_played)/sizeof(g_played[0]); i++)
        g_played[g_played_count++] = buf[i];
}

void snes_pcm_emit(void) {
    if (common_emu_sound_loop_is_muted()) return;

    /* INVARIANT CHECK: at most one emit per DMA period. */
    if (dma_counter == g_last_emit_dma)
        g_double_emit_periods++;
    g_last_emit_dma = dma_counter;
    g_emits_total++;

    int16_t *dst = audio_get_active_buffer();
    uint16_t dst_len = audio_get_buffer_length();
    snes_stretch_pull(dst, dst_len);

    int32_t factor = (int32_t)common_emu_sound_get_volume() + 1;  /* 1..256 */
    for (uint16_t i = 0; i < dst_len; i++)
        dst[i] = (int16_t)(((int32_t)dst[i] * factor) >> 8);

    uint16_t f = snes_stretch_fill();
    if (f > g_max_fill_seen) g_max_fill_seen = f;

    record_output(dst, dst_len);
}

/* snes_pcm_submit in main_snes.c runs the APU to produce one frame of samples
 * and hands them to the stretcher. The test synthesises a recognisable ramp
 * (so a gap is unmistakable) and calls emit once — exactly what the device
 * path does (line 478-479 of main_snes.c). */
static int16_t g_src_counter = 0;
void snes_pcm_submit(void) {
    int16_t buf[SNES_AUDIO_SAMPLES];
    for (uint16_t i = 0; i < SNES_AUDIO_SAMPLES; i++)
        buf[i] = (int16_t)(1000 + (int)(g_src_counter++ % 4000));
    snes_stretch_push(buf, SNES_AUDIO_SAMPLES);
    snes_pcm_emit();
}

/* ---- the pacing block, extracted verbatim from main_snes.c ---------------
 * The shell writes lines from the `if (odroid_system_get_app()...)` marker
 * through the first 4-space-indent closing brace into snes_pacing_block.inc.
 * This file compiles it unmodified: same code, same static local, same
 * SNES_PACE_CATCHUP_WAIT gate. If the block changes in main_snes.c, the test
 * sees the new code on the next run. */
static void run_pacing(void) {
#include "snes_pacing_block.inc"
}

/* ---- frame driver --------------------------------------------------------- */

static unsigned g_min_fill_steady = 0xFFFFFFFF;
static unsigned g_underruns_at_warmup = 0;

static void run_at_fps(double fps, unsigned frames) {
    double frame_ms = 1000.0 / fps;
    unsigned periods_per_frame = (unsigned)(frame_ms / PERIOD_MS);
    /* If the frame is faster than one period the pacing block's initial wait
     * calls cpumon_sleep() once to sync — that path is taken by fast frames
     * (60fps) and is NOT the bug; the bug is the catch-up emit when
     * periods_per_frame >= 2. */
    for (unsigned f = 0; f < frames; f++) {
        /* DMA advanced during frame compute. */
        dma_counter += periods_per_frame;
        g_which_buf ^= (periods_per_frame & 1);
        snes_pcm_submit();
        run_pacing();
        if (f >= 64) {
            unsigned fill = snes_stretch_fill();
            if (fill < g_min_fill_steady) g_min_fill_steady = fill;
        }
    }
}

static void reset_metrics(void) {
    g_last_emit_dma = 0xFFFFFFFF;
    g_double_emit_periods = 0;
    g_emits_total = 0;
    g_max_fill_seen = 0;
    g_min_fill_steady = 0xFFFFFFFF;
    g_played_count = 0;
    g_underruns_at_warmup = 0;
    dma_counter = 0;
    g_which_buf = 0;
    g_src_counter = 0;
    snes_stretch_reset();
}

/* ---- helpers -------------------------------------------------------------- */

static int g_failures = 0;
static void check(int ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_failures++;
}

/* Count the longest run of identical consecutive samples in the STEADY-STATE
 * portion of the played buffer (after warmup). The stretcher holds `last`
 * until the ring reaches TARGET (640 samples), so the first ~3 pulls are
 * silence — that is by design, not a bug, and must not trip this check.
 *
 * The stretcher's dropout filler plays a short loop (64-320 samples) of history
 * — pitch-correct but recognisable as a repeated segment. Real game audio
 * (a ramp that advances every sample) has max_run ≈ 1. A filler-dominated
 * output has max_run ≈ loop_len. */
static unsigned longest_run(void) {
    unsigned skip = 640 + 266;   /* TARGET + one pull past warmup */
    if (g_played_count < skip) return 0;
    unsigned best = 1, cur = 1;
    for (unsigned i = skip + 1; i < g_played_count; i++) {
        if (g_played[i] == g_played[i-1]) { cur++; if (cur > best) best = cur; }
        else cur = 1;
    }
    return best;
}

/* ---- main ----------------------------------------------------------------- */

int main(void) {
    /* fps values chosen to straddle the bug boundary. The catch-up emit only
     * fires when elapsed >= 2, i.e. when a frame spans two or more DMA periods
     * (fps below ~32). 60/50/45/40 must be clean in BOTH versions; 30/28/25/20
     * expose the bug in RED and must be clean in GREEN. */
    static const struct { double fps; const char *label; } cases[] = {
        {60.0, "60 fps (1 period/frame)"},
        {50.0, "50 fps"},
        {45.0, "45 fps"},
        {40.0, "40 fps"},
        {35.0, "35 fps"},
        {30.0, "30 fps (2 periods/frame) -- bug threshold"},
        {28.3, "28.3 fps (SMK device-measured)"},
        {25.0, "25 fps"},
        {20.0, "20 fps (3 periods/frame)"},
    };

    printf("SNES audio pacing block (compiled from main_snes.c)\n");
    printf("DMA period = %.3f ms, buffer = %u samples\n\n", PERIOD_MS, DMA_PERIOD_SAMPLES);

    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset_metrics();
        printf("== %s ==\n", cases[i].label);
        run_at_fps(cases[i].fps, 600);
        g_underruns_at_warmup = snes_stretch_underruns();

        unsigned underruns = snes_stretch_underruns();
        unsigned run = longest_run();
        printf("  emits=%u double_emit_periods=%u min_fill=%u max_fill=%u "
               "underruns=%u longest_run=%u\n",
               g_emits_total, g_double_emit_periods, g_min_fill_steady,
               g_max_fill_seen, underruns, run);

        /* THE invariant: no period receives two emits. */
        check(g_double_emit_periods == 0,
              "at most one emit per DMA period");

        /* Ring must not run dry in steady state. The pacing fix's contract is
         * "stale audio for the missed period" — NOT "ring drained, filler
         * takes over". Underruns after warm-up mean the pacing pulled more
         * than the core pushed. */
        check(underruns == 0,
              "stretcher ring does not run dry in steady state");

        /* Filler detector: a long run of identical samples means the dropout
         * loop is dominating the output. A ramp source has run ≈ 1; the
         * stretcher's shortest loop is 64 samples. Anything over ~100 is
         * filler-dominated. */
        check(run < 100,
              "output is game audio, not dropout filler");
        printf("\n");
    }

    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
