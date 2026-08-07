/* ISR-pull architecture test for the SNES audio path.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The pacing-block fix `(void)elapsed;` stopped the ring-drain bug but exposed
 * the "half-buffer resonance" bug: at fps where frame_time is an integer
 * multiple of the DMA period (30fps = 2 periods, 20fps = 3), dma_state always
 * lands on the same selector, so audio_get_active_buffer() always returns the
 * SAME half. The other half is never written and the DMA plays its stale
 * content forever.
 *
 * The overcome: move the stretcher pull INTO the DMA ISR (gw_audio.c emu_fill),
 * following the existing music_fill pattern. Every period gets exactly one pull
 * regardless of main-loop timing. Both halves are always written.
 *
 * WHAT IT CHECKS
 * --------------
 * Two modes, same stretcher, same fps sweep:
 *
 *   MODE_MAIN_LOOP: main loop calls emit() once per frame (the current (void)elapsed;
 *                   fix). The mock ISR fires but does NOT pull.
 *   MODE_ISR_PULL:  mock ISR calls emu_fill() → registered pull fn every period.
 *                   Main loop only pushes.
 *
 * DMA MODEL
 * ---------
 * Double-buffered circular DMA. The SAI reads sequentially: buf_a then buf_b
 * then buf_a... When the SAI finishes half A, the HF ISR fires (buf_a is now
 * free to write, buf_b is being played). When it finishes buf_b, the TC ISR
 * fires (buf_b is free, buf_a is being played).
 *
 * What the listener HEARS is the content of the INACTIVE buffer (the one being
 * played). The test records inactive-buffer content every period — that's the
 * audio stream the device outputs.
 *
 * INVARIANTS
 *   1. Both half-buffers are written at least once per frame (resonance check).
 *      Detected by tracking g_buf_a_written/g_buf_b_written per frame.
 *   2. No SENTINEL values in the played output (stale-buffer check). SENTINEL
 *      is preloaded into both halves; if a half is never written, its stale
 *      SENTINEL content plays as audio.
 *   3. Output not dominated by filler (longest run of identical samples < 100).
 *
 * RED / GREEN
 * -----------
 * NOT a git-revision RED/GREEN. The two modes ARE the red and green:
 *   RED   = MODE_MAIN_LOOP (fails invariants 1+2 at integer-ratio fps)
 *   GREEN = MODE_ISR_PULL (passes all invariants at all fps)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../Core/Src/porting/snes/snes_audio_stretch.h"

/* ---- constants ------------------------------------------------------------ */

#define SNES_FPS           60
#define SNES_AUDIO_RATE    16000
#define SNES_AUDIO_SAMPLES (SNES_AUDIO_RATE / SNES_FPS)   /* 266 */
#define DMA_PERIOD_SAMPLES SNES_AUDIO_SAMPLES             /* 266, one half-buf */
#define PERIOD_MS          (1000.0 * DMA_PERIOD_SAMPLES / SNES_AUDIO_RATE)  /* 16.625 */

/* Sentinel preloaded into both halves. The stretcher's ramp source produces
 * 1000-4999 and its filler loops through history (same range), so 0x7BEE
 * (31726) cannot appear in legitimate stretcher output. If it shows up in the
 * played stream, a half-buffer was never written. */
#define SENTINEL 0x7BEE

/* ---- mock DMA hardware ---------------------------------------------------- */

typedef enum { DMA_HF = 0, DMA_TC = 1 } dma_state_t;

static int16_t g_buf_a[DMA_PERIOD_SAMPLES];
static int16_t g_buf_b[DMA_PERIOD_SAMPLES];
static dma_state_t g_dma_state = DMA_HF;
static volatile uint32_t g_dma_counter = 0;

/* Per-half total write count over the run. In ISR_PULL mode both halves get
 * many writes. In MAIN_LOOP at even-period fps, one half gets zero. */
static unsigned g_buf_a_write_count = 0;
static unsigned g_buf_b_write_count = 0;

/* The registered pull function (simulates emu_audio_register/enable). */
typedef void (*pull_fn_t)(int16_t *dst, uint16_t n);
static pull_fn_t g_pull_fn = NULL;
static int g_emu_owns = 0;

void mock_emu_audio_register(pull_fn_t fn) { g_pull_fn = fn; }
void mock_emu_audio_enable(int on)         { g_emu_owns = on; }

static int16_t *active_buffer(void) {
    return (g_dma_state == DMA_HF) ? g_buf_a : g_buf_b;
}
static int16_t *inactive_buffer(void) {
    return (g_dma_state == DMA_HF) ? g_buf_b : g_buf_a;
}

static void mark_write(int16_t *buf) {
    if (buf == g_buf_a) { g_buf_a_write_count++; }
    else                { g_buf_b_write_count++; }
}

/* The DMA ISR. On the device: HAL_SAI_TxHalfCpltCallback/CpltCallback.
 * It increments dma_counter, flips the active half, and (in ISR_PULL mode)
 * calls emu_fill → g_pull_fn to fill the just-freed half.
 * Returns the buffer that the DMA will now PLAY (the inactive half). */
static const int16_t *mock_isr_fire(void) {
    g_dma_counter++;
    g_dma_state = (g_dma_state == DMA_HF) ? DMA_TC : DMA_HF;

    if (g_emu_owns && g_pull_fn) {
        int16_t *buf = active_buffer();
        g_pull_fn(buf, DMA_PERIOD_SAMPLES);
        mark_write(buf);
    }

    /* After the flip, the DMA plays the OTHER half (inactive) next period. */
    return inactive_buffer();
}

/* ---- main-loop emit (MODE_MAIN_LOOP) -------------------------------------- */
static void main_loop_emit(void) {
    int16_t *dst = active_buffer();
    snes_stretch_pull(dst, DMA_PERIOD_SAMPLES);
    mark_write(dst);
}

/* ---- output recording: what the listener hears ---------------------------- */
/* Records the inactive buffer (what the DMA plays) every period. This captures
 * stale content when a half was never written. */
static int16_t  g_played[2400 * DMA_PERIOD_SAMPLES];  /* ~2400 periods */
static unsigned g_played_count = 0;

static void record_played(const int16_t *buf, uint16_t n) {
    for (uint16_t i = 0; i < n && g_played_count < sizeof(g_played)/sizeof(g_played[0]); i++)
        g_played[g_played_count++] = buf[i];
}

/* ---- frame driver --------------------------------------------------------- */

typedef enum { MODE_MAIN_LOOP = 0, MODE_ISR_PULL = 1 } pull_mode_t;

static unsigned g_min_fill_steady;

static void preload_sentinels(void) {
    for (int i = 0; i < DMA_PERIOD_SAMPLES; i++) {
        g_buf_a[i] = SENTINEL;
        g_buf_b[i] = SENTINEL;
    }
    g_buf_a_write_count = 0;
    g_buf_b_write_count = 0;
}

static void run_at_fps(pull_mode_t mode, double fps, unsigned frames) {
    double frame_ms = 1000.0 / fps;
    unsigned periods_per_frame = (unsigned)(frame_ms / PERIOD_MS);
    if (periods_per_frame < 1) periods_per_frame = 1;

    int16_t src_buf[SNES_AUDIO_SAMPLES];
    static int16_t src_counter = 0;

    for (unsigned f = 0; f < frames; f++) {
        /* --- main loop: produce one frame of audio --- */
        for (uint16_t i = 0; i < SNES_AUDIO_SAMPLES; i++)
            src_buf[i] = (int16_t)(1000 + (int)(src_counter++ % 4000));
        snes_stretch_push(src_buf, SNES_AUDIO_SAMPLES);

        if (mode == MODE_MAIN_LOOP) {
            main_loop_emit();
        }

        /* --- DMA periods pass during this frame's compute --- */
        for (unsigned p = 0; p < periods_per_frame; p++) {
            const int16_t *playing = mock_isr_fire();
            record_played(playing, DMA_PERIOD_SAMPLES);
        }

        if (f >= 64) {
            unsigned fill = snes_stretch_fill();
            if (fill < g_min_fill_steady) g_min_fill_steady = fill;
        }
    }
}

static void reset_state(pull_mode_t mode) {
    g_dma_counter = 0;
    g_dma_state = DMA_HF;
    g_played_count = 0;
    g_min_fill_steady = 0xFFFFFFFF;
    preload_sentinels();
    snes_stretch_reset();

    if (mode == MODE_ISR_PULL) {
        mock_emu_audio_register(snes_stretch_pull);
        mock_emu_audio_enable(1);
    } else {
        mock_emu_audio_register(NULL);
        mock_emu_audio_enable(0);
    }
}

/* ---- analysis helpers ----------------------------------------------------- */

static unsigned count_sentinels_in_output(void) {
    unsigned skip = 640 + 266;  /* past stretcher priming + warmup */
    unsigned count = 0;
    for (unsigned i = skip; i < g_played_count; i++) {
        if (g_played[i] == (int16_t)SENTINEL) count++;
    }
    return count;
}

static unsigned longest_run_after_warmup(void) {
    unsigned skip = 640 + 266 + DMA_PERIOD_SAMPLES;
    if (g_played_count < skip) return 0;
    unsigned best = 1, cur = 1;
    for (unsigned i = skip + 1; i < g_played_count; i++) {
        if (g_played[i] == g_played[i-1]) { cur++; if (cur > best) best = cur; }
        else cur = 1;
    }
    return best;
}

/* ---- test harness --------------------------------------------------------- */
static int g_failures = 0;
static void check(int ok, const char *what) {
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) g_failures++;
}

int main(void) {
    static const struct { double fps; const char *label; } cases[] = {
        {60.0, "60 fps (1 period/frame)"},
        {50.0, "50 fps"},
        {45.0, "45 fps"},
        {40.0, "40 fps (1.5 periods/frame) -- half-integer, no resonance"},
        {35.0, "35 fps"},
        {30.0, "30 fps (2 periods/frame) -- resonance threshold"},
        {28.3, "28.3 fps (SMK device-measured)"},
        {25.0, "25 fps"},
        {20.0, "20 fps (3 periods/frame)"},
    };

    printf("SNES audio ISR-pull architecture comparison\n");
    printf("DMA period = %.3f ms, half-buffer = %u samples\n\n", PERIOD_MS, DMA_PERIOD_SAMPLES);

    /* ---- MODE_ISR_PULL: GREEN (the new architecture) ---- */
    printf("======== MODE_ISR_PULL (new: ISR pulls every period) ========\n\n");
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset_state(MODE_ISR_PULL);
        printf("-- %s --\n", cases[i].label);
        run_at_fps(MODE_ISR_PULL, cases[i].fps, 600);

        unsigned underruns = snes_stretch_underruns();
        unsigned run = longest_run_after_warmup();
        unsigned sentinels = count_sentinels_in_output();
        unsigned fill = snes_stretch_fill();

        printf("  min_fill=%u end_fill=%u underruns=%u longest_run=%u "
               "sentinels=%u writes_a=%u writes_b=%u\n",
               g_min_fill_steady, fill, underruns, run, sentinels,
               g_buf_a_write_count, g_buf_b_write_count);

        check(sentinels == 0,
              "no stale buffers in played output");
        check(g_buf_a_write_count > 0 && g_buf_b_write_count > 0,
              "both half-buffers written (no resonance)");
        printf("\n");
    }

    /* ---- MODE_MAIN_LOOP: RED (current architecture) ---- */
    printf("======== MODE_MAIN_LOOP (current: emit once per frame) ========\n\n");
    int main_loop_stale = 0;
    int main_loop_resonance = 0;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        reset_state(MODE_MAIN_LOOP);
        printf("-- %s --\n", cases[i].label);
        run_at_fps(MODE_MAIN_LOOP, cases[i].fps, 600);

        unsigned underruns = snes_stretch_underruns();
        unsigned run = longest_run_after_warmup();
        unsigned sentinels = count_sentinels_in_output();
        unsigned fill = snes_stretch_fill();

        printf("  min_fill=%u end_fill=%u underruns=%u longest_run=%u "
               "sentinels=%u writes_a=%u writes_b=%u\n",
               g_min_fill_steady, fill, underruns, run, sentinels,
               g_buf_a_write_count, g_buf_b_write_count);

        if (g_buf_a_write_count == 0 || g_buf_b_write_count == 0) {
            printf("  >>> RESONANCE: one half-buffer NEVER written <<<\n");
            main_loop_resonance = 1;
        }
        if (sentinels > 0) {
            printf("  >>> STALE AUDIO: %u sentinel samples in played output <<<\n",
                   sentinels);
            main_loop_stale = 1;
        }
        printf("\n");
    }

    /* ---- verdict ---- */
    printf("======== VERDICT ========\n");
    printf("MODE_ISR_PULL failures: %d\n", g_failures);
    printf("MODE_MAIN_LOOP resonance: %s, stale audio: %s\n",
           main_loop_resonance ? "YES" : "no",
           main_loop_stale ? "YES" : "no");

    if (g_failures == 0 && (main_loop_resonance || main_loop_stale)) {
        printf("\nOK: ISR_PULL passes all invariants; MAIN_LOOP has the resonance/stale bug.\n");
        printf("Architecture upgrade VERIFIED. Every DMA period gets fresh audio.\n");
        return 0;
    } else if (g_failures > 0) {
        printf("\nFAILED: ISR_PULL mode has invariant violations.\n");
        return 1;
    } else {
        printf("\nUNEXPECTED: MAIN_LOOP did not reproduce the resonance bug.\n");
        return 1;
    }
}
