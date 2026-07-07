#pragma once
/* Host-test stub for rg_alarm.h. The clock tests #include rg_clock.c whole; it
 * now calls a few rg_alarm helpers. Provide them as tiny inline stand-ins so the
 * clock tests need no extra sources or link deps. The real logic is exercised
 * separately by tests/test_alarm.c (which compiles the real rg_alarm.c). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RG_ALARM_MAX 8
typedef struct { uint16_t mins[RG_ALARM_MAX]; int count; bool dnd; } rg_alarm_query_t;

enum { RG_TONE_BEEP = 0, RG_TONE_BEEP2, RG_TONE_CHIRP, RG_TONE_SIREN, RG_TONE_COUNT };

static inline int rg_tone_preset_from_token(const char *tok)
{
    static const char *const T[RG_TONE_COUNT] = { "beep", "beep2", "chirp", "siren" };
    if (tok) for (int i = 0; i < RG_TONE_COUNT; i++) if (strcmp(tok, T[i]) == 0) return i;
    return -1;
}
static inline const char *rg_tone_preset_token(int p)
{
    static const char *const T[RG_TONE_COUNT] = { "beep", "beep2", "chirp", "siren" };
    return T[(p >= 0 && p < RG_TONE_COUNT) ? p : 0];
}
static inline void rg_alarm_cache_refresh(void) { }

/* defined in rg_clock.c itself (the exports under test elsewhere) */
void rg_clock_query_alarms(rg_alarm_query_t *out);
void rg_clock_alarm_prefs(int *preset, int *volume);
