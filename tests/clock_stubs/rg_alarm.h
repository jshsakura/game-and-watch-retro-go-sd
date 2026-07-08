#pragma once
/* Host-test stub for rg_alarm.h. The clock tests #include rg_clock.c whole; it
 * now calls a few rg_alarm helpers. Provide them as tiny inline stand-ins so the
 * clock tests need no extra sources or link deps. The real logic is exercised
 * separately by tests/test_alarm.c (which compiles the real rg_alarm.c). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define RG_ALARM_MAX 8
typedef struct { uint16_t mins[RG_ALARM_MAX]; int count; bool dnd; } rg_alarm_query_t;

enum { RG_TONE_BEEP = 0, RG_TONE_BEEP2, RG_TONE_CHIRP, RG_TONE_SIREN, RG_TONE_COUNT };

static inline int rg_tone_preset_from_token(const char *tok)
{
    static const char *const T[RG_TONE_COUNT] = { "beep", "beep2", "chirp", "siren" };
    if (tok) for (int i = 0; i < RG_TONE_COUNT; i++) if (strcasecmp(tok, T[i]) == 0) return i;
    return -1;
}
static inline const char *rg_tone_preset_token(int p)
{
    static const char *const T[RG_TONE_COUNT] = { "beep", "beep2", "chirp", "siren" };
    return T[(p >= 0 && p < RG_TONE_COUNT) ? p : 0];
}
static inline void rg_alarm_cache_refresh(void) { }

/* Faithful stand-in for the real shared tone generator (rg_alarm.c): the clock
 * ring now delegates here, and tests/test_clock_alarm.c exercises the DMA-half
 * sync + volume scaling through it. Mirrors the RG_TONE_BEEP shape (880Hz gated
 * 250/250). The audio_* seams + dma_counter + volume_tbl come from the clock
 * stubs. s_tone_phase / TONE_HZ are exposed so the existing assertions still
 * read the generator's phase. */
#define TONE_HZ 880
static uint32_t s_tone_phase;
static uint32_t s_stub_tone_dma_mark;
static bool     s_stub_tone_on;
static inline void rg_alarm_tone_feed(uint32_t now, bool ringing, int preset, int vol)
{
    (void)preset;
    if (!ringing) { if (s_stub_tone_on) { audio_stop_playing(); s_stub_tone_on = false; } return; }
    if (!s_stub_tone_on) { audio_start_playing(AUDIO_BUFFER_LENGTH); s_stub_tone_on = true;
                           s_tone_phase = 0; s_stub_tone_dma_mark = dma_counter - 1; }
    if (dma_counter == s_stub_tone_dma_mark) return;
    s_stub_tone_dma_mark = dma_counter;
    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    if (vol < 0) vol = 0; if (vol > ODROID_AUDIO_VOLUME_MAX) vol = ODROID_AUDIO_VOLUME_MAX;
    int amp = (16000 * volume_tbl[vol]) >> 8;
    bool on = ((now / 250) % 2) == 0;
    int period = AUDIO_SAMPLE_RATE / TONE_HZ, half = period / 2;
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (on && amp) { s = (s_tone_phase < (uint32_t)half) ? (int16_t)amp : (int16_t)-amp;
                         if (++s_tone_phase >= (uint32_t)period) s_tone_phase = 0; }
        buf[i] = s;
    }
}

/* The clock rolls the resident all-state cache forward when its alarm fires; on
 * the host that cache does not exist, so this is a no-op counter the tests can
 * read if they ever need to assert the roll happened. */
static inline void rg_alarm_cache_advance(void) {}

/* defined in rg_clock.c itself (the exports under test elsewhere) */
void rg_clock_query_alarms(rg_alarm_query_t *out);
void rg_clock_alarm_prefs(int *preset, int *volume);
