/* See snes_audio_stretch.h for why this exists. */
#include "snes_audio_stretch.h"

#include <string.h>

#define RING      SNES_STRETCH_RING
#define RING_MASK (RING - 1u)
_Static_assert((RING & RING_MASK) == 0u, "ring must be a power of two");

/* Target backlog, in samples. ~40 ms at 16 kHz, and deliberately generous:
 * with 1.35 DMA pulls per emulated frame the level swings by a whole frame
 * between a push and the second pull that follows it, so a cushion under two
 * frames lets a momentary dip reach the floor and start holding samples. */
#define TARGET     640u

/* Length of the history loop a dropout plays, in samples. ~4 ms at 16 kHz:
 * long enough to carry pitch instead of buzzing, short enough to read as a
 * stutter rather than an echo. Must be <= TARGET -- priming is what
 * guarantees that much history exists behind rd. */
#define REPEAT     64u
_Static_assert(REPEAT <= TARGET, "a dropout may only loop primed history");

/* Sanity bound on the MEASURED ratio. Not a playback rate: it exists only to
 * keep one pathological pull (a load, a pause) from poisoning the average. */
#define MEASURE_MAX 262144u  /* 4.0x */

/* The playback-rate band, Q16 (1.0 == 65536). THIS is the number you hear.
 *
 * The measured production/consumption ratio is genuinely below 1.0 whenever
 * the core is under 60.2 emulated fps -- 0.739 at 44, 0.68 at 41 -- and this
 * module used to follow it all the way down. Arithmetically that is right and
 * musically it is wrong: reading the ring at 0.68 is the entire soundtrack
 * transposed down a fifth and slowed by a third, for as long as the scene is
 * slow. On the device it was reported the same way on three different games
 * (Zelda, Super Mario World, Mario Kart) and by the same word each time --
 * flat. A listener cannot un-hear it, and unlike a gap it never stops.
 *
 * So the matcher may correct only what nobody can hear. +/-1% is ~17 cents,
 * under the ~20-25 cents where a detune starts to register on sustained
 * notes, and it is still far more than the jitter this module exists to
 * absorb: a pull that lands a fraction of a frame early or late. Past the
 * band the ring runs dry and the pull holds its last sample -- no zero, no
 * click, but the old cadence back. That is the honest outcome: a core at 68%
 * speed has an fps problem, and paying for it in pitch does not fix the
 * audio, it breaks the audio too. */
#define STEP_ONE   65536u
#define STEP_MIN   64881u   /* 0.99x */
#define STEP_MAX   66191u   /* 1.01x */

static int16_t  ring[RING];
static uint16_t rd, wr, fill;
static uint32_t phase;              /* fractional read position, Q16 < 1.0 */
static uint32_t step = STEP_ONE;    /* rate actually used by the pull       */
static uint32_t base = STEP_ONE;    /* measured production/consumption      */
static uint32_t pushed;             /* samples pushed since the last pull   */
static uint32_t warm_in, warm_out;  /* exact running mean while warming up  */
static uint16_t settled;            /* pulls seen                           */
static uint32_t underruns;
static int16_t  last;
static uint8_t  primed;             /* has the ring ever reached TARGET?    */
static uint16_t debt;               /* filler emitted, owed back in dropped samples */
static uint16_t fpos;               /* history cursor used while dry (rd unmoved)   */
static uint8_t  dry;                /* in a dropout right now?                      */

void snes_stretch_reset(void) {
  memset(ring, 0, sizeof(ring));
  rd = wr = fill = 0;
  phase = 0;
  step = base = STEP_ONE;
  pushed = 0;
  warm_in = warm_out = 0;
  settled = 0;
  underruns = 0;
  last = 0;
  primed = 0;
  debt = 0;
  fpos = 0;
  dry = 0;
}

void snes_stretch_push(const int16_t *src, uint16_t n) {
  for (uint16_t i = 0; i < n; i++) {
    if (fill >= RING) {            /* core outrunning the DMA: drop oldest */
      rd = (uint16_t)((rd + 1u) & RING_MASK);
      fill--;
    }
    ring[wr] = src[i];
    wr = (uint16_t)((wr + 1u) & RING_MASK);
    fill++;
  }
  pushed += n;
  /* Start playing only once a whole TARGET backlog exists. Priming earlier
   * let the reader start while the rate estimate was still 1.0 and drained
   * the ring before it converged; one extra frame of startup silence -- which
   * is what a fresh ROM sounds like anyway -- buys all of that back. */
  if (!primed && fill >= TARGET) primed = 1;
}

/* Set the read rate from what was MEASURED, not from an integrator hunting
 * for it. Between two pulls the core pushed `pushed` samples and the DMA is
 * about to eat `n`, so the ratio that holds the backlog still is exactly
 * pushed/n: 266/266 = 1.0 at 60 fps, 266/(1.353*266) = 0.739 at 44. Neither
 * needs an fps measurement, and neither needs to be told the core slowed down.
 *
 * The averaging is where the care goes. `inst` alternates between a whole
 * frame pushed and nothing, on a ~3.8-pull period, so an EMA fast enough to
 * converge quickly also TRACKS that alternation instead of averaging it: a
 * 1/2 EMA swung the rate between 0.37 and 0.68 and the peaks drained the
 * ring, and a 1/8 EMA still swung ~3%. So an exact running mean until the
 * answer is known, then a slow EMA to follow real speed changes. Jitter here
 * is not a stability curiosity -- it is audible pitch wobble. */
static void retune(uint16_t n) {
  if (n == 0) return;

  /* Deliberately no LOWER clamp on inst: the zero-push pulls are half of what
   * makes the mean correct, and clamping them to STEP_MIN biased it upward
   * (0.82 measured where the answer is 0.739), which made the reader outrun
   * the writer. The final step is clamped instead, where a clamp cannot skew
   * an average. */
  uint32_t inst = (uint32_t)(((uint64_t)pushed << 16) / n);
  if (inst > MEASURE_MAX) inst = MEASURE_MAX;

  if (settled < 128u) {
    warm_in  += pushed;
    warm_out += n;
    settled++;
    base = (uint32_t)(((uint64_t)warm_in << 16) / (warm_out ? warm_out : 1u));
    if (base > MEASURE_MAX) base = MEASURE_MAX;
  } else {
    base = base - (base >> 5) + (inst >> 5);       /* EMA, 1/32 */
  }
  pushed = 0;

  /* Level term: nudge the backlog back toward TARGET, bounded to a few
   * percent so it can never become the thing you hear. */
  int32_t err  = (int32_t)fill - (int32_t)TARGET;
  int32_t corr = err * (int32_t)(base >> 6) / (int32_t)TARGET;
  int32_t lim  = (int32_t)(base / 16u);
  if (corr >  lim) corr =  lim;
  if (corr < -lim) corr = -lim;

  int32_t next = (int32_t)base + corr;
  if (next < (int32_t)STEP_MIN) next = (int32_t)STEP_MIN;
  if (next > (int32_t)STEP_MAX) next = (int32_t)STEP_MAX;
  step = (uint32_t)next;
}

void snes_stretch_pull(int16_t *dst, uint16_t n) {
  retune(n);

  for (uint16_t i = 0; i < n; i++) {
    /* Not primed yet (fresh ROM): hold. At startup that is silence, which is
     * what a fresh ROM sounds like. */
    if (!primed) {
      dst[i] = last;
      continue;
    }

    /* Dry: the core did not produce enough for this period, and the rate band
     * deliberately will not close the gap by slowing the music down. Three
     * things are wanted from what goes out instead, and the first two attempts
     * each bought one by losing another:
     *
     *   hold one sample        no drift, no click, but a DC step -- buzz for
     *                          the ~25% of samples a deficit this size costs
     *   rewind rd and replay   no buzz, right pitch, but the read position
     *                          falls further into the past on every dry
     *                          sample, ~80 times a second, and the stream
     *                          ends up minutes behind the game. That is the
     *                          "밀린다" reported from the device.
     *
     * The rewind was right about the filler and wrong about who pays for it.
     * Filler inserted into a stream delays everything behind it; the only way
     * to insert without delaying is to throw away as much real audio as you
     * inserted. So: play history WITHOUT moving rd (the samples behind it are
     * still in the ring -- reads advance rd, they never clear it), count the
     * debt, and pay it down by dropping real samples once they arrive. Pitch
     * is exact, the read position never moves backwards, latency is bounded,
     * and a slow scene sounds like a stutter -- which is what it is. */
    if (fill < 2u) {
      /* Emit filler from the history BEHIND rd -- reads advance rd, they never
       * clear the ring, so those samples are still there -- but do not move rd
       * to do it. Moving rd backwards is what drifted: the read position falls
       * further into the past on every dry sample, ~80 times a second, and the
       * stream ends up minutes behind the game. Instead record a debt. */
      if (!dry) { dry = 1; fpos = (uint16_t)((rd - REPEAT) & RING_MASK); }
      underruns++;
      if (debt < 0xffffu) debt++;
      int16_t v = ring[fpos];
      fpos = (uint16_t)((fpos + 1u) & RING_MASK);
      if (fpos == rd) fpos = (uint16_t)((rd - REPEAT) & RING_MASK);
      dst[i] = v;
      continue;
    }
    dry = 0;

    /* Pay the debt down with real samples: every sample of filler that went
     * out has to cost one sample of stream, or the filler IS the drift. This
     * is the whole difference between "the sound stutters on a slow scene"
     * and "the sound falls behind and never comes back". */
    while (debt && fill > 2u) {
      rd = (uint16_t)((rd + 1u) & RING_MASK);
      fill--;
      debt--;
    }
    if (fill < 2u) { dst[i] = last; continue; }

    int32_t s0 = ring[rd];
    int32_t s1 = ring[(rd + 1u) & RING_MASK];
    int32_t v  = s0 + (((s1 - s0) * (int32_t)(phase >> 1)) >> 15);
    last = (int16_t)v;
    dst[i] = (int16_t)v;

    phase += step;
    while (phase >= 65536u && fill >= 1u) {
      phase -= 65536u;
      rd = (uint16_t)((rd + 1u) & RING_MASK);
      fill--;
    }
  }
}

uint16_t snes_stretch_fill(void)      { return fill; }
uint32_t snes_stretch_step_q16(void)  { return step; }
uint32_t snes_stretch_underruns(void) { return underruns; }
