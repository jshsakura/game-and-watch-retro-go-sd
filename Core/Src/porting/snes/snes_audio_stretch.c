/* See snes_audio_stretch.h for why this exists. */
#include "snes_audio_stretch.h"

#include <string.h>

/* When the pull runs in the DMA ISR (emu_audio_enable path), push and pull
 * share fill / rd / pushed / primed. The push runs in main-loop context and
 * guards the shared fields with a brief IRQ disable. On the host (tests) there
 * is no ISR, so the macros are no-ops. */
#ifdef TARGET_GNW
/* main.h for the CMSIS intrinsics. Without it these are implicit declarations
 * that the host build never sees, because there they are macros -- the device
 * build is the one that fails, and that is the lie CLAUDE.md warns about.
 *
 * And SAVE/RESTORE means what it says: __enable_irq() unconditionally would
 * turn interrupts back ON at the end of a section that some caller had already
 * disabled them around. Save PRIMASK, restore PRIMASK. */
#include "main.h"
#define STRETCH_IRQ_SAVE()    const uint32_t irq_state_ = __get_PRIMASK(); __disable_irq()
#define STRETCH_IRQ_RESTORE() __set_PRIMASK(irq_state_)
#else
#define STRETCH_IRQ_SAVE()    ((void)0)
#define STRETCH_IRQ_RESTORE() ((void)0)
#endif

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
/* Splice crossfade, samples. The seam of a repeated segment is a click, and a
 * click every loop_len samples is a tone -- 250 Hz with the old fixed 64. */
#define XFADE      16u
/* Loop-length search range for a dropout: 50 Hz (320 samples) down to 250 Hz
 * (64) at 16 kHz. A loop that is a whole number of waveform periods splices
 * without a seam; one that is not cannot be crossfaded into sounding right. */
#define LOOP_MIN   64u
#define LOOP_MAX   320u
/* Insertions between autocorrelation searches. See the call site. */
#define PICK_EVERY 8u
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
static volatile uint16_t rd, wr, fill;
static uint32_t phase;              /* fractional read position, Q16 < 1.0 */
static uint32_t step = STEP_ONE;    /* rate actually used by the pull       */
static uint32_t base = STEP_ONE;    /* measured production/consumption      */
static volatile uint32_t pushed;    /* samples pushed since the last pull   */
static uint32_t warm_in, warm_out;  /* exact running mean while warming up  */
static uint16_t settled;            /* pulls seen                           */
static uint32_t underruns;
static int16_t  last;
static volatile uint8_t  primed;    /* has the ring ever reached TARGET?    */
static uint16_t picks_since = PICK_EVERY;  /* frames since the last search */
static volatile uint16_t pitch_meas;       /* measured by push, read by the ISR */
static uint16_t stretch_pick_period(void); /* defined below; push calls it first */
static int32_t  fill_lp;            /* low-passed fill level for corr (see retune)  */
static int32_t  time_error;         /* push/pull deficit: +ve = behind (insert)     */
static uint16_t pitch_est = REPEAT; /* current pitch period for insertion           */
static int16_t  ins_buf[LOOP_MAX];  /* one period ready to emit without consuming   */
static uint16_t ins_pos, ins_len;   /* insertion buffer cursor / length             */

void snes_stretch_reset(void) {
  picks_since = PICK_EVERY;   /* first push after a load searches */
  pitch_meas = 0;
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
  fill_lp = (int32_t)TARGET;
  time_error = 0;
  pitch_est = REPEAT;
  ins_pos = ins_len = 0;
}

void snes_stretch_push(const int16_t *src, uint16_t n) {
  /* Main-loop context: this is where the expensive measurement belongs. Once
   * every PICK_EVERY frames is plenty -- a melody's period does not move
   * between one frame and the next eight. */
  if (primed && ++picks_since >= PICK_EVERY) {
    picks_since = 0;
    pitch_meas = stretch_pick_period();
  }
  STRETCH_IRQ_SAVE();
  for (uint16_t i = 0; i < n; i++) {
    if (fill >= RING) {            /* core outrunning the DMA: drop oldest */
      rd = (uint16_t)((rd + 1u) & RING_MASK);
      fill--;
    }
    ring[wr] = src[i];
    wr = (uint16_t)((wr + 1u) & RING_MASK);
    fill++;
  }
  /* Overflow drain -- the mirror of the dry filler, and it was missing.
   *
   * A WSOLA insertion adds output samples without consuming ring samples, so on
   * a game that runs below 60 fps the backlog grows every frame and never comes
   * back: measured on the device at 1817 samples against a target of 640, which
   * is 114 ms of the sound arriving late, with the playback rate sitting
   * clamped at its floor and unable to drain it. The old guard only fired when
   * the ring was completely FULL, so the latency ceiling was the ring size
   * rather than the target.
   *
   * When the core is persistently slow there is no way to keep latency bounded
   * except to throw audio away -- playing everything it produced at the right
   * pitch means falling behind real time by exactly the deficit, for ever. Drop
   * whole pitch periods so the splice lands where the waveform repeats. */
  if (fill > TARGET * 2u) {
    uint16_t p = pitch_meas ? pitch_meas : REPEAT;
    while (fill > TARGET + p) {
      rd = (uint16_t)((rd + p) & RING_MASK);
      fill = (uint16_t)(fill - p);
    }
  }
  pushed += n;
  uint8_t should_prime = (!primed && fill >= TARGET) ? 1 : 0;
  STRETCH_IRQ_RESTORE();
  /* Start playing only once a whole TARGET backlog exists. Priming earlier
   * let the reader start while the rate estimate was still 1.0 and drained
   * the ring before it converged; one extra frame of startup silence -- which
   * is what a fresh ROM sounds like anyway -- buys all of that back. */
  if (should_prime) primed = 1;
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
   * percent so it can never become the thing you hear.
   *
   * The instantaneous fill swings by a whole frame (±~266) between the pull
   * that follows a push and the one that doesn't, on a ~3.8-pull period.
   * Feeding that swing straight into corr drove step between STEP_MIN and
   * STEP_MAX on successive pulls — a ±1% pitch alternation at pull rate
   * (60 Hz), reported as an audible buzz ("웅웅거린다"). Low-pass the fill
   * level at 1/8: the per-pull jitter shrinks to ±33 samples (corr ±0.08%,
   * inaudible), while real drift still converges in ~8 pulls (~130 ms). */
  fill_lp += ((int32_t)fill - fill_lp) >> 3;
  int32_t err  = fill_lp - (int32_t)TARGET;
  int32_t corr = err * (int32_t)(base >> 6) / (int32_t)TARGET;
  int32_t lim  = (int32_t)(base / 16u);
  if (corr >  lim) corr =  lim;
  if (corr < -lim) corr = -lim;

  int32_t next = (int32_t)base + corr;
  if (next < (int32_t)STEP_MIN) next = (int32_t)STEP_MIN;
  if (next > (int32_t)STEP_MAX) next = (int32_t)STEP_MAX;
  step = (uint32_t)next;
}

/* Pick the repeat length for a dropout: the lag in [LOOP_MIN, LOOP_MAX] whose
 * history best matches the most recent samples. That makes the loop a whole
 * number of waveform periods, so the seam falls where the waveform repeats
 * anyway and the crossfade has something to hide. Searched decimated by 2 in
 * both lag and sample -- this runs once per dropout, not per sample. */
static uint16_t stretch_pick_period(void) {
  const uint16_t win = 128u;                 /* samples compared per lag */
  int64_t best_score = INT64_MIN;
  uint16_t best_lag = REPEAT;

  for (uint16_t lag = LOOP_MIN; lag <= LOOP_MAX; lag += 2u) {
    int64_t acc = 0;
    for (uint16_t k = 0; k < win; k += 2u) {
      int32_t a = ring[(uint16_t)((rd - 1u - k) & RING_MASK)];
      int32_t b = ring[(uint16_t)((rd - 1u - k - lag) & RING_MASK)];
      acc += (int64_t)a * b;
    }
    /* Normalise by lag so a long lag does not win merely by summing more
     * energy -- it does not here (window is fixed), but keep the shorter loop
     * when scores tie: shorter means the dropout reads as a stutter, longer
     * as an echo. */
    if (acc > best_score) { best_score = acc; best_lag = lag; }
  }
  return best_lag;
}

void snes_stretch_pull(int16_t *dst, uint16_t n) {
  uint32_t was_pushed = pushed;
  retune(n);
  time_error += (int32_t)n - (int32_t)was_pushed;

  for (uint16_t i = 0; i < n; i++) {
    if (!primed) {
      dst[i] = last;
      continue;
    }

    if (ins_pos < ins_len) {
      dst[i] = ins_buf[ins_pos++];
      continue;
    }

    if (time_error >= (int32_t)pitch_est && fill > pitch_est + 2u) {
      /* The search is an autocorrelation over 129 lags and it runs in the SAI
       * interrupt. A game with a large deficit inserts on nearly every pull --
       * Mario Kart is 43% short at 34 fps -- so recomputing it every time puts
       * the whole search in the ISR 60 times a second. A melody's period does
       * not change between one insertion and the next 8; reuse it, and pay for
       * the search once per PICK_EVERY insertions. */
      /* No search here. The autocorrelation is 129 lags of 64-sample MACs and
       * this runs in the SAI interrupt; an on-device PC profile put 45.9% of
       * the whole machine inside this one loop, more than the emulator itself.
       * push() measures the period in main-loop context instead and leaves it
       * in pitch_meas; the interrupt only reads it. */
      if (pitch_meas) pitch_est = pitch_meas;
      if (pitch_est < LOOP_MIN) pitch_est = LOOP_MIN;
      for (uint16_t k = 0; k < pitch_est; k++)
        ins_buf[k] = ring[(uint16_t)((rd - pitch_est + k) & RING_MASK)];
      uint16_t xf = pitch_est < XFADE ? pitch_est : XFADE;
      for (uint16_t k = 0; k < xf; k++) {
        int32_t nrml = ring[(uint16_t)((rd + k) & RING_MASK)];
        ins_buf[k] = (int16_t)(((int32_t)ins_buf[k] * (int32_t)(xf - k) +
                                nrml * (int32_t)(k + 1u)) / (int32_t)(xf + 1u));
      }
      time_error -= (int32_t)pitch_est;
      ins_pos = 0;
      ins_len = pitch_est;
      dst[i] = ins_buf[ins_pos++];
      continue;
    }

    if (fill < 2u) {
      underruns++;
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
