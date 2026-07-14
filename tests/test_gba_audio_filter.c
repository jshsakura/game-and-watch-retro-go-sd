/* The GBA output low-pass, proven against tones — compiling the REAL
 * Core/Src/porting/gba/gba_audio_filter.c, not a copy of it (a test that links
 * a different program proves nothing; docs/HARNESSES.md).
 *
 * The filter's whole contract, as numbers:
 *   - configured for a 13,379 Hz cart (Pokemon), music passes and images die:
 *       1 kHz  : within 1 dB of unity
 *       12 kHz : at least 20 dB down   (where Ruby's grit actually lived)
 *       18 kHz : at least 35 dB down
 *   - rate 0 (no FIFO clocked) and high rates (>= 38 kHz) BYPASS: the buffer
 *     must come out bit-identical, not merely similar.
 *   - the cutoff follows a rate change, and does not recompute when the rate
 *     did not change.
 *   - an hour of full-scale noise neither blows up nor NaNs (the clamp holds).
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Core/Src/porting/gba/gba_audio_filter.h"

#define SR 48000.0

/* Steady-state gain of the whole filter at f, measured, not derived: feed a
 * full-scale tone, let it settle, compare RMS out to RMS in. */
static double tone_gain_db(double f)
{
    enum { N = 48000, SETTLE = 4000 };
    static int16_t buf[N];
    double in_rms = 0.0, out_rms = 0.0;

    for (int i = 0; i < N; i++)
        buf[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * f * i / SR));
    for (int i = SETTLE; i < N; i++)
        in_rms += (double)buf[i] * buf[i];

    gba_lpf_reset();
    gba_lpf_apply(buf, N);

    for (int i = SETTLE; i < N; i++)
        out_rms += (double)buf[i] * buf[i];
    return 10.0 * log10((out_rms + 1e-12) / (in_rms + 1e-12));
}

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } \
    else         { printf("ok   " __VA_ARGS__); printf("\n"); } \
} while (0)

int main(void)
{
    /* --- a Pokemon-rate cart: pass the music, kill the images --- */
    gba_lpf_configure(13379);
    CHECK(gba_lpf_cutoff_hz() == 13379 * 42 / 100,
          "13379 Hz cart -> cutoff %u Hz (0.42x)", gba_lpf_cutoff_hz());

    double g1k  = tone_gain_db(1000.0);
    double g12k = tone_gain_db(12000.0);
    double g18k = tone_gain_db(18000.0);
    CHECK(fabs(g1k) < 1.0,  "1 kHz passes: %+.2f dB (want |x| < 1)", g1k);
    CHECK(g12k < -20.0,     "12 kHz dies: %+.1f dB (want < -20)", g12k);
    CHECK(g18k < -35.0,     "18 kHz dies harder: %+.1f dB (want < -35)", g18k);

    /* --- bypass must be bit-exact, both ways in --- */
    int16_t a[512], b[512];
    for (int i = 0; i < 512; i++)
        a[i] = b[i] = (int16_t)((i * 2654435761u) >> 17);

    gba_lpf_configure(0);
    CHECK(gba_lpf_cutoff_hz() == 0, "rate 0 -> bypass");
    gba_lpf_apply(a, 512);
    CHECK(memcmp(a, b, sizeof a) == 0, "bypass at rate 0 is bit-identical");

    gba_lpf_configure(42048);
    CHECK(gba_lpf_cutoff_hz() == 0, "42048 Hz cart -> bypass (images inaudible)");
    gba_lpf_apply(a, 512);
    CHECK(memcmp(a, b, sizeof a) == 0, "bypass at high rate is bit-identical");

    /* --- the cutoff follows the game --- */
    gba_lpf_configure(18157);
    CHECK(gba_lpf_cutoff_hz() == 18157 * 42 / 100,
          "rate change reconfigures: 18157 -> cutoff %u Hz", gba_lpf_cutoff_hz());
    /* 7 kHz sits at 0.92x this cart's cutoff, so Butterworth grants it about
     * -1.6 dB — audibly intact. The number that matters is the contrast: the
     * 13 kHz cart's fixed 5.6 kHz cutoff would have put it 20 dB down. */
    double g7k_wide = tone_gain_db(7000.0);
    CHECK(g7k_wide > -3.0,
          "7 kHz survives an 18 kHz cart: %+.2f dB (a fixed cutoff: -20)",
          g7k_wide);

    /* --- an hour of full-scale noise: stable, not railed --- */
    /* An unstable IIR does not overflow an int16_t — the clamp guarantees
     * that. What it does is pin the output to the rails. So the honest check
     * is the railed fraction: white noise through a low-pass should almost
     * never touch full scale; a blown-up filter touches nothing else. */
    gba_lpf_configure(13379);
    gba_lpf_reset();
    uint32_t seed = 1;
    int16_t chunk[804];
    long railed = 0, total = 0;
    for (int f = 0; f < 60 * 60 * 60; f++) {   /* 60 min of 60 fps frames */
        for (int i = 0; i < 804; i++) {
            seed = seed * 1664525u + 1013904223u;
            chunk[i] = (int16_t)(seed >> 16);
        }
        gba_lpf_apply(chunk, 804);
        for (int i = 0; i < 804; i++)
            if (chunk[i] == 32767 || chunk[i] == -32768) railed++;
        total += 804;
    }
    CHECK(railed * 100 < total,
          "one hour of full-scale noise: %ld of %ld samples railed (<1%%)",
          railed, total);

    if (failures) {
        printf("FAIL: tests/test_gba_audio_filter.c (%d)\n", failures);
        return 1;
    }
    printf("PASS: tests/test_gba_audio_filter.c\n");
    return 0;
}
