/* Adaptive audio stretcher for the SNES core.
 *
 * The problem it exists for
 * -------------------------
 * The core produces exactly SNES_AUDIO_SAMPLES (266 = 16000/60) samples per
 * EMULATED frame, and the audio DMA consumes one buffer every 16.625 ms of
 * REAL time. Those two only balance at 60 emulated fps. Mario Kart runs at
 * ~44, so the DMA drains 1.35 buffers per frame while the core refills one,
 * and main_snes.c's pacing block says what happens next in as many words:
 * "LLE accepts the underrun (stale audio for one DMA period on slow frames)".
 * That stale period IS the audible gap. The device profile shows it exactly:
 * deadline advance was 1 on 34 frames and 2 on 30, never 0.
 *
 * Why not just emulate more APU
 * -----------------------------
 * Because the same comment explains why that was rejected: extra apu_cycle()
 * calls advance the SPC700's timer relative to the 65816 and change the music
 * tempo. This module never touches the SPC700. It takes the samples the core
 * already produced and resamples them across the real time that actually
 * elapsed. The emulated machine is untouched; only the playback rate moves.
 *
 * What it costs
 * -------------
 * The output is pitched down by exactly the speed deficit — at 44 fps the
 * music plays at 0.74x. That is not a distortion being introduced: the game
 * is ALREADY running at 0.74x, and the video with it. This makes the audio
 * agree with the video instead of stuttering at the wrong rate.
 *
 * At full speed the loop converges to step == 1.0 and the pull is a straight
 * copy, so a 60 fps scene is unaffected.
 */
#pragma once

#include <stdint.h>

/* Ring capacity in samples. 2048 = ~7.7 frames of emulated audio, which is
 * far more headroom than the +-1 buffer of jitter the pacing block produces;
 * the control loop holds the level near STRETCH_TARGET, not near full. */
#define SNES_STRETCH_RING 2048u

void     snes_stretch_reset(void);

/* Push one emulated frame's worth of samples (producer side). */
void     snes_stretch_push(const int16_t *src, uint16_t n);

/* Fill dst with n samples (consumer side). ALWAYS writes n samples — that is
 * the whole point; it never zero-pads a short frame the way the old path did. */
void     snes_stretch_pull(int16_t *dst, uint16_t n);

/* Diagnostics for the device profiler / tests. */
uint16_t snes_stretch_fill(void);
uint32_t snes_stretch_step_q16(void);
uint32_t snes_stretch_underruns(void);
