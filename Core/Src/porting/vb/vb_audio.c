/*
 * Virtual Boy audio bridge — VB VSU (stereo, 50 kHz) -> retro-go mono mixer.
 *
 * Uses the SAME technique already in the device for the Atari Lynx (also a
 * stereo handheld): the core generates stereo frames, we downmix L+R -> mono,
 * apply the in-game volume, and clamp to the last generated frame when the
 * generated count doesn't exactly match the DMA buffer length (rate jitter).
 * See Core/Src/porting/lynx/main_lynx.cpp sound_store().
 *
 * Split into its own unit (like vb_savestate.c) so the host harness verifies the
 * bridge produces non-silent, correctly-scaled output on the device path.
 *
 * The red-viper core calls sound_push_backend() when a SAMPLE_COUNT-frame stereo
 * buffer completes; main_vb.c calls vb_audio_drain() once per emulated frame.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "vb_sound.h"   /* SAMPLE_COUNT, sound_push_backend prototype */

/* Headroom for one emulated frame (50000/50 = 1000 stereo frames) + slack. */
#define VB_AUDIO_MAX_FRAMES 2048

static int16_t s_accum[VB_AUDIO_MAX_FRAMES * 2];  /* L,R interleaved */
static int     s_frames;

/* Called by vb_sound.c: a completed SAMPLE_COUNT-frame stereo buffer. Return
 * non-zero so the core advances to the next wave buffer. */
bool sound_push_backend(int16_t *buf)
{
    int n = SAMPLE_COUNT;
    if (s_frames + n > VB_AUDIO_MAX_FRAMES)
        n = VB_AUDIO_MAX_FRAMES - s_frames;
    if (n > 0) {
        memcpy(&s_accum[s_frames * 2], buf, (size_t)n * 2 * sizeof(int16_t));
        s_frames += n;
    }
    return true;
}

int vb_audio_pending(void) { return s_frames; }

void vb_audio_reset(void) { s_frames = 0; }

/* Downmix the frames accumulated this emulated frame into the caller's mono
 * buffer out[0..len), scaling by `factor` (volume, >>9 as in the Lynx core).
 * Drains the accumulator. */
void vb_audio_drain(int16_t *out, int len, int32_t factor)
{
    int gen = s_frames;
    if (gen <= 0) {
        /* Nothing generated this frame -> silence (not a stale repeat). */
        memset(out, 0, (size_t)len * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < len; i++) {
        int idx = (i < gen) ? i : gen - 1;   /* clamp to last frame when gen < len */
        int32_t s = (int32_t)s_accum[idx * 2] + (int32_t)s_accum[idx * 2 + 1];
        out[i] = (int16_t)((s * factor) >> 9);
    }
    s_frames = 0;
}
