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

/* Step limits, Q16 (1.0 == 65536). The clamp keeps a pathological scene (a
 * load, a pause) from driving the rate somewhere unrecognisable; it degrades
 * to a bounded stretch plus held samples rather than anything unbounded. */
#define STEP_ONE   65536u
#define STEP_MIN   16384u   /* 0.25x */
#define STEP_MAX   262144u  /* 4.0x  */

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
  if (inst > STEP_MAX) inst = STEP_MAX;

  if (settled < 128u) {
    warm_in  += pushed;
    warm_out += n;
    settled++;
    base = (uint32_t)(((uint64_t)warm_in << 16) / (warm_out ? warm_out : 1u));
    if (base > STEP_MAX) base = STEP_MAX;
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
    /* Not primed yet (fresh ROM) or the producer genuinely stalled: hold the
     * last value. At startup that is silence, which is correct; mid-run it is
     * a held sample, inaudible where a zero would be a click -- and a zero is
     * exactly what the old path wrote. */
    if (!primed || fill < 2u) {
      if (primed) underruns++;
      dst[i] = last;
      continue;
    }

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
