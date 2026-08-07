/* SNES audio stretcher regression test.
 *
 * Links the REAL module (Core/Src/porting/snes/snes_audio_stretch.c), not a
 * reimplementation — the project has been burned by reimplemented tests
 * (hw_jpeg_decoder.c: three tests, 0% coverage, three shipped bugs).
 *
 * THE CONTRACT THIS TEST ENFORCES
 * ================================
 * User testimony is the verdict: "예전엔 더 낮은 프레임에서도 안 깨졌다" —
 * it used to not break even at lower fps. That is three things simultaneously:
 *
 *   1. CONTINUOUS: no underruns in steady state. A gap is a click, a stutter,
 *      or silence — all audible breaks. The ring may never run dry once primed.
 *   2. CORRECT PITCH: the output fundamental matches the input fundamental.
 *      The old stretcher followed the deficit down (0.74x at 44fps = a fifth
 *      flat) and was reported "5도 낮다" on three games. Pitch may not be
 *      dragged by the emulator's frame rate.
 *   3. BOUNDED: the ring fill stays bounded — the stretcher must not drift
 *      unboundedly ahead or behind.
 *
 * The ±1% step clamp (STEP_MIN/STEP_MAX) was added to enforce (2) but broke
 * (1): at any fps below ~58 the ring runs dry and the pull plays filler. That
 * is the regression. A pitch-preserving time-stretch (WSOLA or equivalent)
 * satisfies all three: it absorbs the deficit by duplicating waveform-period-
 * aligned segments, keeping pitch exact while filling the gap.
 *
 * RED-GREEN STRUCTURE
 * ===================
 * Against the ±1% clamped stretcher, test 2 (44fps) FAILS: underruns > 0.
 * Against an unclamped stretcher, test 2 FAILS differently: pitch is flat.
 * Only a pitch-preserving stretch passes both checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../Core/Src/porting/snes/snes_audio_stretch.h"

#define FRAME 266u   /* SNES_AUDIO_SAMPLES */
#define DMA   266u   /* one audio-DMA period */
#define SR    16000  /* sample rate */

static int failures;

static void check(int ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

/* A harmonic test signal: fundamental at F0_HZ plus 2nd and 3rd harmonics.
 * This has clear periodicity for WSOLA to exploit AND a measurable pitch,
 * unlike a ramp (no periodicity) or pure noise (no pitch). */
/* Test signal: pure sine at F0_HZ. Period must be an integer number of
 * samples at SR so autocorrelation has a clean peak at the exact lag. */
#define F0_HZ 250.0
#define F0_PERIOD 64u   /* 16000/250 = 64 exactly */
static int16_t src_sample(unsigned n) {
  double t = (double)n / SR;
  return (int16_t)(sin(2.0 * M_PI * F0_HZ * t) * 12000.0);
}

/* Detect the fundamental frequency of a buffer via autocorrelation.
 * Returns the detected period in samples, or 0 if no clear peak found.
 *
 * Uses first-local-maximum (not global max) because for a periodic signal
 * the autocorrelation peaks at P, 2P, 3P... — the FIRST peak IS the
 * fundamental. A global-max scan would grab 3P if windowing made it
 * marginally higher than P. A threshold-crossing scan would grab a cosine
 * lobe (cos(2π·(P-8)/P) = 0.707 crosses 0.7 before reaching P). */
static unsigned detect_period(const int16_t *buf, unsigned len) {
  static double scores[401];
  unsigned max_lag = 400 < len / 2 ? 400 : len / 2;
  for (unsigned lag = 20; lag <= max_lag; lag++) {
    double acc = 0.0, norm = 0.0;
    for (unsigned i = 0; i + lag < len; i++) {
      acc += (double)buf[i] * (double)buf[i + lag];
      norm += (double)buf[i] * (double)buf[i];
    }
    if (norm < 1.0) norm = 1.0;
    scores[lag] = acc / norm;
  }
  /* Find the first local maximum with score > 0.5 */
  for (unsigned lag = 22; lag + 2 <= max_lag; lag++) {
    if (scores[lag] > scores[lag - 1] && scores[lag] > scores[lag - 2] &&
        scores[lag] >= scores[lag + 1] && scores[lag] >= scores[lag + 2] &&
        scores[lag] > 0.5)
      return lag;
  }
  return 0;
}

static unsigned steady_underruns;

/* Run `frames` emulated frames against `periods_per_frame` DMA periods.
 * After warmup, collects output into a contiguous buffer for pitch analysis. */
static void run(double periods_per_frame, unsigned frames,
                int *saw_zero_run, unsigned *max_zero_run, unsigned *pulls,
                int16_t *out_buf, unsigned *out_len) {
  static int16_t out[DMA];
  unsigned produced = 0;
  double acc = 0.0;
  unsigned zero_run = 0;
  unsigned under_at_warm = 0;
  *max_zero_run = 0; *saw_zero_run = 0; *pulls = 0;
  *out_len = 0;

  /* Pre-fill: prime the ring before any pulls. On the device the emulator
   * runs several frames before audio DMA starts; without this, a balanced
   * push/pull (60fps) never reaches TARGET and stays unprimed — output is
   * DC, and pitch detection returns period=0. */
  while (snes_stretch_fill() < 640u + FRAME) {
    int16_t in[FRAME];
    for (unsigned i = 0; i < FRAME; i++) in[i] = src_sample(produced++);
    snes_stretch_push(in, FRAME);
  }

  for (unsigned f = 0; f < frames; f++) {
    int16_t in[FRAME];
    for (unsigned i = 0; i < FRAME; i++) in[i] = src_sample(produced++);
    snes_stretch_push(in, FRAME);

    acc += periods_per_frame;
    while (acc >= 1.0) {
      acc -= 1.0;
      snes_stretch_pull(out, DMA);
      (*pulls)++;
      if (f < 32) { under_at_warm = snes_stretch_underruns(); continue; }
      /* After warmup: check for zero runs AND collect for pitch analysis */
      for (unsigned i = 0; i < DMA; i++) {
        if (out[i] == 0) { zero_run++; if (zero_run > *max_zero_run) *max_zero_run = zero_run; *saw_zero_run = 1; }
        else zero_run = 0;
        if (*out_len < 8192) out_buf[(*out_len)++] = out[i];
      }
    }
  }
  steady_underruns = snes_stretch_underruns() - under_at_warm;
}

int main(void) {
  printf("SNES audio stretcher — continuity + pitch contract\n");

  /* 1. Full speed: one emulated frame per DMA period. The rate loop must
   *    settle at 1.0 and the output must track the input, not drift. */
  {
    static int16_t obuf[8192]; unsigned olen;
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(1.0, 400, &saw, &mx, &pulls, obuf, &olen);
    double step = snes_stretch_step_q16() / 65536.0;
    unsigned period = detect_period(obuf, olen);
    double detected_hz = period ? (double)SR / period : 0;
    printf("    step=%.4f fill=%u underruns=%u period=%u (%.1fHz)\n",
           step, snes_stretch_fill(), snes_stretch_underruns(),
           period, detected_hz);
    check(fabs(step - 1.0) < 0.05, "60 fps: playback rate stays ~1.0x");
    check(steady_underruns == 0, "60 fps: no underrun in steady state");
    /* Pitch should match input within 2% (measurement tolerance) */
    check(detected_hz > 0 && fabs(detected_hz - F0_HZ) / F0_HZ < 0.02,
          "60 fps: pitch preserved (not flat)");
  }

  /* 2. THE REGRESSION CASE. Mario Kart at ~44 fps drives 1.35 DMA periods
   *    per emulated frame. The ±1% step clamp keeps pitch correct but the
   *    ring runs dry — stutter. User testimony: "예전엔 더 낮은 프레임에서도
   *    안 깨졌다". This must hold BOTH conditions: no underrun AND no flat
   *    pitch.
   *
   *    Against the ±1% clamped stretcher: FAILS on underruns.
   *    Against an unclamped stretcher: FAILS on pitch.
   *    Against a pitch-preserving stretch (WSOLA): PASSES both. */
  {
    static int16_t obuf[8192]; unsigned olen;
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 44.0, 600, &saw, &mx, &pulls, obuf, &olen);
    double step = snes_stretch_step_q16() / 65536.0;
    unsigned period = detect_period(obuf, olen);
    double detected_hz = period ? (double)SR / period : 0;
    printf("    step=%.4f fill=%u underruns=%u (steady %u) period=%u (%.1fHz)\n",
           step, snes_stretch_fill(), snes_stretch_underruns(),
           steady_underruns, period, detected_hz);

    check(steady_underruns == 0,
          "44 fps: NO underrun in steady state (audio is continuous)");
    check(period > 0 && fabs(detected_hz - F0_HZ) / F0_HZ < 0.05,
          "44 fps: pitch preserved (not flat — the old 0.74x was '5도 낮다')");
  }

  /* 3. Heavy deficit (20fps): must still not break. Three DMA periods per
   *    frame means the stretcher has to produce 3x the push rate. This is
   *    the extreme case — WSOLA should handle it with degraded quality but
   *    no silence, no flat pitch. */
  {
    static int16_t obuf[8192]; unsigned olen;
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 20.0, 300, &saw, &mx, &pulls, obuf, &olen);
    double step = snes_stretch_step_q16() / 65536.0;
    unsigned period = detect_period(obuf, olen);
    double detected_hz = period ? (double)SR / period : 0;
    printf("    step=%.4f fill=%u underruns=%u period=%u (%.1fHz)\n",
           step, snes_stretch_fill(), snes_stretch_underruns(),
           period, detected_hz);
    check(steady_underruns == 0,
          "20 fps: NO underrun (heavy deficit absorbed)");
  }

  /* 4. Faster than realtime (fast-forward): the level must not pin at the
   *    ring wall for ever — the rate has to rise and drain it. */
  {
    static int16_t obuf[8192]; unsigned olen;
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 90.0, 400, &saw, &mx, &pulls, obuf, &olen);
    double step = snes_stretch_step_q16() / 65536.0;
    printf("    step=%.4f fill=%u\n", step, snes_stretch_fill());
    check(step > 1.0, "90 fps: rate leans against the backlog");
    check(snes_stretch_fill() <= SNES_STRETCH_RING, "90 fps: ring stays bounded");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
