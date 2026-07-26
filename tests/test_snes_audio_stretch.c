/* Unit test for the SNES audio stretcher.
 *
 * It compiles and links the REAL module (Core/Src/porting/snes/
 * snes_audio_stretch.c), not a reimplementation of it — the project has been
 * burned by tests that reimplemented what they claimed to cover
 * (hw_jpeg_decoder.c: three tests, 0% coverage, three shipped bugs).
 *
 * The three things that have to hold:
 *  1. At full speed the stretcher is transparent.
 *  2. Below full speed it NEVER emits a gap. This is the whole reason the
 *     module exists: the old path wrote 266 samples and zero-filled the rest
 *     of the DMA buffer, which is an audible click every slow frame.
 *  3. Above full speed it does not run away — the level comes back down.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../Core/Src/porting/snes/snes_audio_stretch.h"

#define FRAME 266u   /* SNES_AUDIO_SAMPLES */
#define DMA   266u   /* one audio-DMA period */

static int failures;

static void check(int ok, const char *what) {
  printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

/* A recognisable signal: a slow ramp so interpolation errors are obvious and
 * a silent gap is unmistakable (the ramp never returns to zero mid-run). */
static int16_t src_sample(unsigned n) {
  return (int16_t)(1000 + (int)(n % 4000));
}

/* Run `frames` emulated frames against `periods_per_frame` DMA periods and
 * report the worst gap seen. Fractional rates are handled by accumulating. */
/* Underruns during the first frames are not a defect: the ring starts empty
 * after a ROM load and holding silence there is the correct behaviour. What
 * must be zero is underruns in STEADY STATE, so the counter is sampled once
 * the warm-up is over and only the delta is judged. */
static unsigned steady_underruns;

static void run(double periods_per_frame, unsigned frames,
                int *saw_zero_run, unsigned *max_zero_run, unsigned *pulls) {
  static int16_t out[DMA];
  unsigned produced = 0;
  double acc = 0.0;
  unsigned zero_run = 0;
  unsigned under_at_warm = 0;
  *max_zero_run = 0; *saw_zero_run = 0; *pulls = 0;

  for (unsigned f = 0; f < frames; f++) {
    int16_t in[FRAME];
    for (unsigned i = 0; i < FRAME; i++) in[i] = src_sample(produced++);
    snes_stretch_push(in, FRAME);

    acc += periods_per_frame;
    while (acc >= 1.0) {
      acc -= 1.0;
      snes_stretch_pull(out, DMA);
      (*pulls)++;
      /* Ignore the very first pulls: the ring starts empty by construction,
       * exactly as it does after a ROM load, and a few held samples there are
       * correct behaviour, not a gap in steady state. */
      if (f < 32) { under_at_warm = snes_stretch_underruns(); continue; }
      for (unsigned i = 0; i < DMA; i++) {
        if (out[i] == 0) { zero_run++; if (zero_run > *max_zero_run) *max_zero_run = zero_run; *saw_zero_run = 1; }
        else zero_run = 0;
      }
    }
  }
  steady_underruns = snes_stretch_underruns() - under_at_warm;
}

int main(void) {
  printf("SNES audio stretcher\n");

  /* 1. Full speed: one emulated frame per DMA period. The rate loop must
   *    settle at 1.0 and the output must track the input, not drift. */
  {
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(1.0, 400, &saw, &mx, &pulls);
    double step = snes_stretch_step_q16() / 65536.0;
    printf("    step=%.4f fill=%u underruns=%u\n",
           step, snes_stretch_fill(), snes_stretch_underruns());
    check(fabs(step - 1.0) < 0.05, "60 fps: playback rate stays ~1.0x");
    check(!saw, "60 fps: no silent gap");
    check(steady_underruns == 0, "60 fps: no underrun in steady state");
  }

  /* 2. The real case. Mario Kart at ~44 fps drives 1.35 DMA periods per
   *    emulated frame. This is precisely the condition under which the old
   *    path zero-filled, so a gap here means the module does not do its job. */
  {
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 44.4, 600, &saw, &mx, &pulls);
    double step = snes_stretch_step_q16() / 65536.0;
    printf("    step=%.4f fill=%u underruns=%u (steady %u) pulls=%u\n",
           step, snes_stretch_fill(), snes_stretch_underruns(), steady_underruns, pulls);
    check(!saw, "44 fps: no silent gap (the bug this module exists for)");
    check(steady_underruns == 0, "44 fps: never runs dry in steady state");
    /* The stretch must match the speed deficit, or the backlog would grow
     * without bound and latency with it. 44.4/60.15 = 0.738. */
    check(fabs(step - 44.4 / 60.15) < 0.06, "44 fps: rate tracks the speed deficit");
  }

  /* 3. A very slow scene (a load, a heavy transition) must degrade, not
   *    explode: the clamp holds and it still never emits a zero. */
  {
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 20.0, 300, &saw, &mx, &pulls);
    printf("    step=%.4f fill=%u underruns=%u\n",
           snes_stretch_step_q16() / 65536.0, snes_stretch_fill(),
           snes_stretch_underruns());
    check(!saw, "20 fps: still no silent gap");
  }

  /* 4. Faster than realtime (fast-forward): the level must not pin at the
   *    ring wall for ever — the rate has to rise and drain it. */
  {
    snes_stretch_reset();
    int saw; unsigned mx, pulls;
    run(16000.0 / 266.0 / 90.0, 400, &saw, &mx, &pulls);
    double step = snes_stretch_step_q16() / 65536.0;
    printf("    step=%.4f fill=%u\n", step, snes_stretch_fill());
    check(step > 1.05, "90 fps: rate rises to drain the backlog");
    check(snes_stretch_fill() < SNES_STRETCH_RING, "90 fps: ring does not pin full");
  }

  printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
