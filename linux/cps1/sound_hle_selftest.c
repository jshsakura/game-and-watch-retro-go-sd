/*
 * Build-test for cps1_sound_hle.c in isolation (no CPU/bus involved): a
 * tone channel actually oscillates near its programmed frequency, an OKI
 * sample plays back sample-for-sample and then auto-silences, multiple
 * channels sum linearly, and an overflowing mix clips instead of wrapping.
 *
 *   ./build/cps1-sound-hle-selftest
 */
#include <stdio.h>

#include "cps1_sound_hle.h"

static int failures = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                          \
            failures++;                                                     \
        }                                                                    \
    } while (0)

int main(void)
{
    cps1_sound_hle_t snd;

    /* --- tone frequency: command 0x04 = note 1, channel 0 -> 240Hz --- */
    {
        cps1_sound_hle_reset(&snd);
        cps1_sound_hle_trigger(&snd, 0x04);
        uint32_t duration = CPS1_SOUND_SAMPLE_RATE * 250u / 1000u;
        static int16_t buf[6000];
        cps1_sound_hle_mix(&snd, buf, duration);

        int transitions = 0;
        for (uint32_t i = 1; i < duration; i++)
            if ((buf[i] > 0) != (buf[i - 1] > 0))
                transitions++;
        double expected = 2.0 * 240.0 * ((double)duration / CPS1_SOUND_SAMPLE_RATE);
        printf("[cps1-sound-hle-selftest] tone: transitions=%d expected~=%.1f\n",
               transitions, expected);
        CHECK(transitions > (int)(expected * 0.9) && transitions < (int)(expected * 1.1),
              "tone transitions %d out of range of expected %.1f", transitions, expected);
        CHECK(snd.tone[0].enabled == 0, "tone channel should auto-disable after its duration");
    }

    /* --- OKI: exact sample-for-sample playback, then auto-silence --- */
    {
        cps1_sound_hle_reset(&snd);
        static const int8_t sample[] = { 10, -10, 20, -20, 5 };
        cps1_sound_hle_attach_oki_sample(&snd, sample, 5);
        cps1_sound_hle_trigger(&snd, 0x80);
        int16_t buf[6];
        cps1_sound_hle_mix(&snd, buf, 6);
        for (int i = 0; i < 5; i++) {
            int16_t expect = (int16_t)(sample[i] * 256);
            CHECK(buf[i] == expect, "oki sample %d: expected %d got %d", i, expect, buf[i]);
        }
        CHECK(buf[5] == 0, "oki should be silent once its sample is exhausted, got %d", buf[5]);
        CHECK(snd.oki.enabled == 0, "oki channel should auto-disable after playback");
    }

    /* --- linearity: two channels at phase 0 sum exactly --- */
    {
        cps1_sound_hle_reset(&snd);
        cps1_sound_hle_trigger(&snd, 0x04); /* note1, ch0 */
        cps1_sound_hle_trigger(&snd, 0x05); /* note1, ch1 */
        int16_t buf[1];
        cps1_sound_hle_mix(&snd, buf, 1);
        printf("[cps1-sound-hle-selftest] ch0+ch1 sample0=%d (expect 16000)\n", buf[0]);
        CHECK(buf[0] == 16000, "two channels at phase 0 should sum to 16000, got %d", buf[0]);
    }

    /* --- clipping: 4 tone channels + a loud OKI sample must clip, not wrap --- */
    {
        cps1_sound_hle_reset(&snd);
        static const int8_t loud[] = { 127 };
        cps1_sound_hle_attach_oki_sample(&snd, loud, 1);
        cps1_sound_hle_trigger(&snd, 0x80);
        for (unsigned ch = 0; ch < CPS1_SOUND_TONE_CHANNELS; ch++)
            cps1_sound_hle_trigger(&snd, (uint8_t)((1u << 2) | ch));
        int16_t buf[1];
        cps1_sound_hle_mix(&snd, buf, 1);
        printf("[cps1-sound-hle-selftest] clip sample0=%d (expect 32767)\n", buf[0]);
        CHECK(buf[0] == 32767, "overflowing mix should clip to INT16_MAX, got %d", buf[0]);
    }

    if (failures) {
        fprintf(stderr, "[cps1-sound-hle-selftest] FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-sound-hle-selftest] OK\n");
    return 0;
}
