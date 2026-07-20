#include <stddef.h>

#include "cps1_sound_hle.h"

#define CPS1_SOUND_TONE_AMPLITUDE     8000
#define CPS1_SOUND_TONE_DURATION_MS   250u

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

void cps1_sound_hle_mix(cps1_sound_hle_t *snd, int16_t *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t acc = 0;

        for (int c = 0; c < CPS1_SOUND_TONE_CHANNELS; c++) {
            cps1_sound_tone_channel_t *t = &snd->tone[c];
            if (!t->enabled)
                continue;

            acc += (t->phase < 0x80000000u) ? t->amplitude : (int32_t)(-t->amplitude);
            t->phase += t->phase_step;

            if (t->samples_remaining > 0) {
                t->samples_remaining--;
                if (t->samples_remaining == 0)
                    t->enabled = 0;
            }
        }

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
