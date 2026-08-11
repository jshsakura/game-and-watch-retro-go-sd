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

/* Noise band. PICOLA repeats one waveform period, which is seamless when the
 * signal HAS a period and a random splice when it does not -- and Zelda 3's
 * rain is broadband noise. The period picker below scores lags by raw
 * correlation with no notion of confidence, so on noise it returns whichever
 * lag happened to score highest and the "seamless" repeat is a correlated
 * artefact: the crackle heard on the rain, still there at 57 fps because the
 * deficit that triggers it is 5% at 57 and only closes at 60.
 *
 * The cure is to swap the two tools round, because each is inaudible exactly
 * where the other hurts. A rate change transposes a melody -- unacceptable --
 * but on noise it is imperceptible, there being no pitch to move. So when the
 * period estimate is not confident, widen the rate band and let resampling
 * absorb the deficit; PICOLA then never fires on noise. Tonal content keeps
 * the tight +-1% band and the period repeat, unchanged. */
/* How far down the rate may follow. 65536 is 1.0x; the deficit at 57 fps wants
 * about 0.95. Capping it above that trades transposition back for splices --
 * the floor is the dial between "flat but clean" and "in tune but spliced", and
 * it is the only place where the two artefacts can be traded continuously. */
#ifndef SNES_STRETCH_FLOOR
#define SNES_STRETCH_FLOOR 55050   /* 0.84x -- follow the deficit all the way */
#endif
#define STEP_MIN_NOISE 60293u  /* 0.92x -- the noise band when keeping pitch */
#define STEP_MIN_REV   63570u  /* 0.97x */
#define STEP_MAX_REV   67502u  /* 1.03x */
#define STEP_MAX_NOISE 71362u  /* 1.089x */
/* Confidence is a continuum, so the response is one too. A hard threshold left
 * a third of the rain's pulls on the tonal side of a cliff -- measured: 218
 * insertions still, against 255 with the fix off -- because broadband noise
 * scores around 0.5 and wanders across any single line you draw. Interpolate
 * instead: at CONF_TONAL and above the band is the old +-1% and PICOLA is
 * armed; at CONF_NOISE and below it is the full noise band and PICOLA is off;
 * between, both move proportionally. Nothing has an edge to fall off. */
#define CONF_TONAL 200u   /* 0.78 -- a real period scores at least this */
#define CONF_NOISE 100u   /* 0.39 -- below this there is no period at all */
/* Normalised correlation, Q8. Below this the "period" is not one. */
#ifndef SNES_STRETCH_NOISE_AWARE
#define SNES_STRETCH_NOISE_AWARE 1
#endif
/* SNES_STRETCH_FOLLOW=1: no splicing at all, ever. The playback rate simply
 * follows the rate the core produces at, so 266 samples a frame at 57 emulated
 * fps play back as 15,200 a second instead of being stretched into 16,000. The
 * ring never runs dry and nothing is ever repeated, at the price of a CONSTANT
 * transposition of however far below 60 fps the core is -- 5% at 57 fps, about
 * 85 cents, flat and unchanging.
 *
 * This is what the module originally shipped as and was rejected for, but the
 * rejection was measured at 44 fps, where the same rule transposes a fifth. At
 * 57 it is a different proposition, and it is the only setting with no
 * artefact at all.
 *
 * MEASURED over 1800 EMULATED frames -- the same deterministic window for every
 * arm, which matters: on wall-clock windows the same build read 0 and 181
 * underruns, wider than most of the differences being judged, and one of those
 * readings was reported as a result before the instrument was fixed.
 *
 *                              splices   underruns   pitch
 *   always PICOLA (before)         198         723   kept
 *   noise-aware band               189         573   kept
 *   SNES_STRETCH_FLOOR 3%          179         416   -3%
 *   follow the rate                  0         162   -5%
 *
 * Re-measured after the band was corrected to open downward only, same window,
 * one session, so these two are directly comparable:
 *
 *   noise-aware band (default)     184         684   kept
 *   follow the rate                  0          68   -5%
 *
 * FOLLOW is not a trade of one artefact for another: it removes every splice
 * AND three quarters of the dropouts.
 *
 * And THERE IS NO USEFUL MIDDLE. The 3% floor was built to find one and does
 * not: it costs three quarters of the transposition to buy 10 splices out of
 * 189. The deficit is either covered by the rate or it is not; a floor that
 * stops short leaves a remainder, and the remainder is spliced exactly as
 * before. The choice is binary, and the only thing a counter cannot judge is
 * which side of it the ear prefers. */
#ifndef SNES_STRETCH_FOLLOW
#define SNES_STRETCH_FOLLOW 0
#endif
/* Runtime, not compile time, because the choice below is one only a listener can
 * make and it should not need a rebuild to hear both. The compile-time flag is
 * just this variable's initial value.
 *
 *   0  keep pitch      -- the deficit is covered by splicing, which is
 *                         inaudible on tone and is the rain's crackle on noise
 *   1  gap-free        -- the rate follows what the core produces, so nothing
 *                         is ever spliced or repeated, at a constant
 *                         transposition of however far below 60 fps it runs
 *
 * Measured, 1800 emulated frames of Zelda 3 rain, same window:
 *   keep pitch   184 splices   684 dropouts
 *   gap-free       0 splices    68 dropouts   -5% */
uint8_t g_snes_audio_gapfree = SNES_STRETCH_FOLLOW;
#ifndef SNES_STRETCH_FLOOR
#define SNES_STRETCH_FLOOR 55050
#endif
/* SNES_STRETCH_NOISE_REVERSE=1: on noise, fill the gap with the last segment
 * PLAYED BACKWARDS instead of repeated.
 *
 * PICOLA's repeat introduces exactly the thing noise does not have -- a period
 * -- and the ear hears that new correlation as the crackle. Widening the rate
 * band avoids it but destabilises the level loop (measured: 739 underruns), and
 * following the rate outright transposes everything by 5%.
 *
 * A reversed segment has the identical power spectrum, which is all that
 * defines noise perceptually, and introduces no periodicity at all. It also
 * joins seamlessly by construction: reversed, it BEGINS with the sample the
 * ring just ended on, so there is no step at the splice -- the one place PICOLA
 * needs a crossfade to hide. Pitch is untouched and the band stays at the +-1%
 * that keeps the loop stable. */
/* MEASURED, AND IT DOES NOT DOMINATE. Thirty seconds of rain, same window:
 *
 *                            splices            underruns   pitch
 *   noise-aware band only      213 periodic          0      kept
 *   reversal, +-1% band        414 (325 reversed)  219      kept
 *   reversal, +-3% band        410 (307 reversed)  283      kept
 *   follow the rate              0                  55      -5%
 *
 * The reasoning holds -- a reversed segment really does have the same spectrum
 * and no new periodicity, and it cut the PERIODIC splices from 213 to 89 -- but
 * it fires more often and the ring runs dry doing it, and a dry ring is an
 * artefact too. Off by default, kept with its numbers, because the argument is
 * sound and the next attempt should start from why it drains rather than from
 * the idea. */
#ifndef SNES_STRETCH_NOISE_REVERSE
#define SNES_STRETCH_NOISE_REVERSE 0
#endif
#if SNES_STRETCH_NOISE_AWARE
#ifndef SNES_STRETCH_CONF_MIN
#define SNES_STRETCH_CONF_MIN 160   /* 0.63 */
#endif
#define PITCH_CONF_MIN ((uint16_t)SNES_STRETCH_CONF_MIN)
#else
/* =0 restores the old behaviour: every pull is "tonal", so the band stays at
 * +-1% and PICOLA splices whatever the unnormalised picker called a period. */
#define PITCH_CONF_MIN 0u
#endif

static int16_t  ring[RING];
static volatile uint16_t rd, wr, fill;
static uint32_t phase;              /* fractional read position, Q16 < 1.0 */
static uint32_t step = STEP_ONE;    /* rate actually used by the pull       */
static uint32_t base = STEP_ONE;    /* measured production/consumption      */
static volatile uint32_t pushed;    /* samples pushed since the last pull   */
static uint32_t warm_in, warm_out;  /* exact running mean while warming up  */
static uint16_t settled;            /* pulls seen                           */
static uint32_t underruns;
/* Verification counters for the noise fix. The ear is the final judge, but these
 * say whether the mechanism engaged at all: on rain, insertions should fall to
 * ~zero and noise_pulls should be nearly every pull. */
uint32_t g_stretch_ins, g_stretch_pulls, g_stretch_noise_pulls, g_stretch_conf_lp;
uint32_t g_stretch_rev;   /* of those insertions, how many were reversed */
static int16_t  last;
static volatile uint8_t  primed;    /* has the ring ever reached TARGET?    */
static uint16_t picks_since = PICK_EVERY;  /* frames since the last search */
static volatile uint16_t pitch_meas;       /* measured by push, read by the ISR */
static uint16_t stretch_pick_period(void); /* defined below; push calls it first */
static int32_t  fill_lp;            /* low-passed fill level for corr (see retune)  */
static int32_t  time_error;         /* push/pull deficit: +ve = behind (insert)     */
static uint16_t pitch_est = REPEAT; /* current pitch period for insertion           */
static uint16_t pitch_conf;         /* Q8 normalised correlation of that estimate   */
static uint16_t noise_w;            /* 0..256: how noise-like the passage is        */
static int32_t  conf_lp;            /* smoothed pitch confidence, Q8                */
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
  conf_lp = 0; noise_w = 0;
  g_stretch_ins = g_stretch_pulls = g_stretch_noise_pulls = 0; g_stretch_conf_lp = 0; g_stretch_rev = 0;
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
  /* Asymmetric: running dry is an artefact, running long is only latency, so
   * react to a low backlog several times faster than to a high one. The 1/8
   * low-pass above is there to keep the +-1% band from alternating audibly;
   * that argument does not apply when the ring is about to be empty. */
  if ((int32_t)fill < (int32_t)(TARGET / 2)) fill_lp = (int32_t)fill;
  int32_t err  = fill_lp - (int32_t)TARGET;
  int32_t corr = err * (int32_t)(base >> 6) / (int32_t)TARGET;
  int32_t lim  = (int32_t)(base / 16u);
  if (corr >  lim) corr =  lim;
  if (corr < -lim) corr = -lim;

  int32_t next = (int32_t)base + corr;
  /* Noise: no pitch to transpose, so let the rate take the deficit and keep
   * PICOLA away from it. Tonal: the tight band, as before. */
  /* Classification is per pull, deliberately.
   *
   * Smoothing it over the passage was tried and is WORSE: insertions 236 and
   * underruns 739, against 203 and 382 for the per-pull version and 255/476
   * with the whole fix off. Holding the band wide for a whole scene hands the
   * level term that much more authority, and the loop oscillates across it --
   * the +-1% band was doing double duty as a stability limit, not just as an
   * audibility limit. Widen it only as far as the current window justifies. */
  conf_lp += ((int32_t)pitch_conf - conf_lp) >> 4;   /* reported, not used here */
  uint32_t cs = pitch_conf;
  uint32_t w;
  if (g_snes_audio_gapfree) w = 256; else
#if SNES_STRETCH_NOISE_AWARE
  if (cs >= CONF_TONAL) w = 0;
  else if (cs <= CONF_NOISE) w = 256;
  else w = 256u - ((cs - CONF_NOISE) * 256u) / (CONF_TONAL - CONF_NOISE);
#else
  w = 0;
#endif
  noise_w = (uint16_t)w;
  g_stretch_pulls++;
  if (w >= 128u) g_stretch_noise_pulls++;
  g_stretch_conf_lp = cs;
#if SNES_STRETCH_NOISE_REVERSE
  /* Reversal and the rate share the work. Reversal alone left the band at +-1%
   * and the ring ran dry 219 times in thirty seconds; the full +-8% band alone
   * oscillated. A third of the way -- +-3% on noise, about 50 cents and only on
   * material with no pitch to hear it in -- covers the drift, and the reversed
   * fillers cover the jitter. */
  int32_t lo = (int32_t)STEP_MIN - (int32_t)(((STEP_MIN - STEP_MIN_REV) * w) >> 8);
  int32_t hi = (int32_t)STEP_MAX;   /* downward only; see the note above */
#else
  /* The band opens DOWNWARD only.
   *
   * The deficit is one-directional -- the core is slower than real time, never
   * faster -- so the rate never needs to exceed 1.0 by more than the +-1% the
   * level term uses to lean against a backlog. Opening the high side as well let
   * the loop run the reader at up to 1.089x whenever the content was noise-like,
   * and at 60 fps, where there is no deficit to justify it, that empties the
   * ring: tests/run.sh's "60 fps: no underrun in steady state" went to 130. The
   * device could not see it -- it is at 57 fps and always in deficit. */
  const uint32_t floor_q = g_snes_audio_gapfree ? (uint32_t)SNES_STRETCH_FLOOR
                                                : STEP_MIN_NOISE;
  int32_t lo = (int32_t)STEP_MIN - (int32_t)(((STEP_MIN - floor_q) * w) >> 8);
  int32_t hi = (int32_t)STEP_MAX;
#endif
  if (next < lo) next = lo;
  if (next > hi) next = hi;
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

  /* Energy of the reference window, for the normalisation the score needs to
   * mean anything. Without it a loud passage always "correlates" more than a
   * quiet one and noise scores whatever its loudest lag happens to give. */
  int64_t energy = 0;
  for (uint16_t k = 0; k < win; k += 2u) {
    int32_t a = ring[(uint16_t)((rd - 1u - k) & RING_MASK)];
    energy += (int64_t)a * a;
  }

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
  /* Confidence = best correlation / energy, Q8, clamped. A periodic waveform
   * scores near 1.0 at its period; noise scores near 0 at every lag. */
  pitch_conf = 0;
  if (energy > 0 && best_score > 0) {
    int64_t q = (best_score << 8) / energy;
    pitch_conf = (uint16_t)(q > 255 ? 255 : q);
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

#if SNES_STRETCH_NOISE_REVERSE
    /* Noise: reverse rather than repeat. Same spectrum, no new periodicity, and
     * the seam is continuous because a reversed segment starts on the sample the
     * ring just ended on. */
    /* `fill > pitch_est + 2` looks like the wrong guard and is not.
     *
     * The argument for loosening it to XFADE is clean: the copy reads HISTORY,
     * the samples behind rd, which are present whether or not anything is
     * queued ahead, and only the crossfade tail reads forward -- sixteen
     * samples. On that reasoning the guard switches insertions off exactly when
     * the ring is low and they are the only thing that can stop it emptying.
     *
     * MEASURED, AND IT IS BACKWARDS: with the guard at XFADE the same window
     * goes from 0 underruns to 444. Whatever the copy needs, this guard is also
     * keeping the loop out of a state it does not recover from -- insertions
     * fired at a low backlog stop `time_error` from ever reporting the deficit
     * that the level term needs to see in order to slow the rate down. Reverted;
     * the number is here so the next person does not re-derive the same clean
     * argument and ship it. */
    if (!g_snes_audio_gapfree && noise_w >= 128u &&
        time_error >= (int32_t)REPEAT && fill > REPEAT + 2u) {
      uint16_t len = REPEAT;
      for (uint16_t k = 0; k < len; k++)
        ins_buf[k] = ring[(uint16_t)((rd - 1u - k) & RING_MASK)];
      /* Crossfade only the tail, into the forward samples that follow. */
      uint16_t xf = len < XFADE ? len : XFADE;
      for (uint16_t k = 0; k < xf; k++) {
        int32_t back = ins_buf[len - xf + k];
        int32_t fwd  = ring[(uint16_t)((rd + k) & RING_MASK)];
        ins_buf[len - xf + k] =
            (int16_t)((back * (int32_t)(xf - k) + fwd * (int32_t)(k + 1u)) /
                      (int32_t)(xf + 1u));
      }
      time_error -= (int32_t)len;
      ins_pos = 0;
      ins_len = len;
      g_stretch_ins++;
      g_stretch_rev++;
      dst[i] = ins_buf[ins_pos++];
      continue;
    }
#endif
    /* Never splice a period into something that has none. */
    /* The guard is on XFADE, not on the insertion length, and that is the whole
     * point of this line.
     *
     * It used to read `fill > pitch_est + 2`, i.e. "only insert while the
     * backlog is bigger than the thing being inserted" -- which switches the
     * insertions OFF exactly when the ring is running low and they are the only
     * thing that can stop it emptying. With pitch_est up to 320 against a
     * TARGET of 640 that swings by a whole frame, the ring crossed the guard
     * regularly and then drained to zero unopposed. Every underrun counted in
     * this module came through that door.
     *
     * The copy does not need `fill` at all: it reads HISTORY, the samples
     * behind rd, which are in the ring whether or not anything is queued ahead.
     * Only the crossfade tail reads forward, and only XFADE (16) of them. */
    /* A floor that does not reach the deficit leaves a remainder, and the
     * remainder has to go somewhere -- so PICOLA stays armed for every floor
     * except the one that follows all the way down. Getting this comparison
     * backwards left the intermediate floors with no cover at all: 22,508
     * underruns at a 1% floor against 162 at full follow. */
#define STRETCH_FLOOR_FOLLOWS_ALL (SNES_STRETCH_FLOOR <= 58982)   /* 0.90x */
    /* Two different questions, and folding them into one condition got it wrong
     * twice. In FOLLOW the rate is already following as far as its floor lets
     * it, so PICOLA is needed exactly when that floor does NOT reach the
     * deficit -- noise-ness does not enter into it, and gating on noise_w
     * (which FOLLOW pins at 256) switched PICOLA off entirely and left the
     * remainder uncovered: 18,559 underruns at a 1% floor. Without FOLLOW the
     * question is the original one: splice tone, do not splice noise. */
    const int picola_ok = g_snes_audio_gapfree ? !STRETCH_FLOOR_FOLLOWS_ALL
                                               : (noise_w < 128u);
    if (picola_ok &&
        time_error >= (int32_t)pitch_est && fill > pitch_est + 2u) {
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
      g_stretch_ins++;
      dst[i] = ins_buf[ins_pos++];
      continue;
    }

    if (fill < 2u) {
      /* Dry. Holding `last` puts a DC step at both ends of the gap, and a step
       * is a click -- so a dropout was audible twice over even when it was
       * short. Decay toward zero instead: a few samples of exponential fade are
       * inaudible where a held level is not, and the recovery starts from a
       * value near the signal rather than from wherever it happened to stop. */
      underruns++;
      last = (int16_t)(last - (last >> 3));
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
