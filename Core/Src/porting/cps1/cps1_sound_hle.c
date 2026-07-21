#include <stddef.h>

#include "cps1_core.h"
#include "cps1_sound_hle.h"

#define CPS1_SOUND_TONE_AMPLITUDE     8000
#define CPS1_SOUND_TONE_DURATION_MS   250u

/* cps1_sound_hle_mix's per-sample loop below is manually unrolled over
 * exactly CPS1_SOUND_TONE_CHANNELS channels -- this guards that unroll
 * against silently going stale if the channel count is ever changed
 * without updating the mix loop to match. */
_Static_assert(CPS1_SOUND_TONE_CHANNELS == 4, "cps1_sound_hle_mix's unrolled channel loop assumes 4 channels");

void cps1_sound_hle_reset(cps1_sound_hle_t *snd)
{
    for (int i = 0; i < CPS1_SOUND_TONE_CHANNELS; i++) {
        snd->tone[i].enabled = 0;
        snd->tone[i].phase = 0;
        snd->tone[i].phase_step = 0;
        snd->tone[i].amplitude = 0;
        snd->tone[i].samples_remaining = 0;
    }
    snd->oki.enabled = 0;
    snd->oki.data = NULL;
    snd->oki.length = 0;
    snd->oki.position = 0;
}

void cps1_sound_hle_attach_oki_sample(cps1_sound_hle_t *snd, const int8_t *data, uint32_t length)
{
    snd->oki.data = data;
    snd->oki.length = length;
    snd->oki.position = 0;
    snd->oki.enabled = 0;
}

void cps1_sound_hle_trigger(cps1_sound_hle_t *snd, uint8_t command)
{
    if (command == 0x00) {
        for (int i = 0; i < CPS1_SOUND_TONE_CHANNELS; i++)
            snd->tone[i].enabled = 0;
        return;
    }

    if (command < 0x80) {
        unsigned ch = command & 0x03u;
        unsigned note = (command >> 2) & 0x1Fu;
        uint32_t freq_hz = 220u + note * 20u;

        cps1_sound_tone_channel_t *t = &snd->tone[ch];
        t->enabled = 1;
        t->phase = 0;
        t->phase_step = (uint32_t)(((uint64_t)freq_hz << 32) / CPS1_SOUND_SAMPLE_RATE);
        t->amplitude = CPS1_SOUND_TONE_AMPLITUDE;
        t->samples_remaining = CPS1_SOUND_SAMPLE_RATE * CPS1_SOUND_TONE_DURATION_MS / 1000u;
        return;
    }

    /* command >= 0x80: (re)trigger the OKI sample from the start. */
    snd->oki.position = 0;
    snd->oki.enabled = (snd->oki.data != NULL && snd->oki.length > 0);
}

/* One tone channel's contribution to a single sample -- identical body to
 * the pre-unroll loop's per-channel logic, called 4 times explicitly (see
 * cps1_sound_hle_mix) instead of via a counted loop, matching the Phase-13
 * optimization-phase ask (removes the loop-control branch/compare/
 * increment CPS1_SOUND_TONE_CHANNELS times per sample). */
static inline int32_t cps1_sound_hle_mix_channel(cps1_sound_tone_channel_t *t)
{
    if (!t->enabled)
        return 0;

    int32_t contribution = (t->phase < 0x80000000u) ? t->amplitude : (int32_t)(-t->amplitude);
    t->phase += t->phase_step;

    if (t->samples_remaining > 0) {
        t->samples_remaining--;
        if (t->samples_remaining == 0)
            t->enabled = 0;
    }
    return contribution;
}

CPS1_ITCM_TEXT
void cps1_sound_hle_mix(cps1_sound_hle_t *snd, int16_t *out, uint32_t count)
{
    cps1_sound_tone_channel_t *t0 = &snd->tone[0];
    cps1_sound_tone_channel_t *t1 = &snd->tone[1];
    cps1_sound_tone_channel_t *t2 = &snd->tone[2];
    cps1_sound_tone_channel_t *t3 = &snd->tone[3];

    for (uint32_t i = 0; i < count; i++) {
        int32_t acc = 0;

        acc += cps1_sound_hle_mix_channel(t0);
        acc += cps1_sound_hle_mix_channel(t1);
        acc += cps1_sound_hle_mix_channel(t2);
        acc += cps1_sound_hle_mix_channel(t3);

        if (snd->oki.enabled) {
            acc += (int32_t)snd->oki.data[snd->oki.position] * 256;
            snd->oki.position++;
            if (snd->oki.position >= snd->oki.length)
                snd->oki.enabled = 0;
        }

        if (acc > 32767)  acc = 32767;
        if (acc < -32768) acc = -32768;
        out[i] = (int16_t)acc;
    }
}
