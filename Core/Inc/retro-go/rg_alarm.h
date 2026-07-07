#pragma once

/* All-state alarm: a resident "next alarm" cache plus the shared pieces that let
 * the clock's alarm ring EVERYWHERE (launcher, in-game, music/video, deep sleep)
 * instead of only inside the clock app. Deliberately tiny — the only resident
 * RAM is a couple of ints; everything heavy (parse, ring UI) reuses code that
 * already ships. See Core/Src/retro-go/rg_alarm.c.
 *
 * Split into a PURE part (next-alarm math, wake-cause decision, tone-preset
 * tables — host-unit-tested in tests/test_alarm.c) and a firmware part (RTC
 * backup mirror, RTC Alarm A arming, the SAI tone feed, the in-place ring).
 * The firmware part is compiled out with -DRG_ALARM_HOST so the pure logic can
 * be tested on the host without any HAL. */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* ---- snapshot of the alarm-relevant clock config -----------------------
 * Filled by rg_clock_query_alarms() (defined in rg_clock.c, so it reuses the
 * clock's own cfg parser — no second parser). Only enabled alarms are listed. */
#define RG_ALARM_MAX 8
typedef struct {
    uint16_t mins[RG_ALARM_MAX];   /* minute-of-day (hour*60+min) of each enabled alarm */
    int      count;
    bool     dnd;                  /* Do-Not-Disturb: suppresses every alarm */
} rg_alarm_query_t;

/* Implemented in rg_clock.c (reuses clock_config_load's parser). */
void rg_clock_query_alarms(rg_alarm_query_t *out);
/* The synth-beep preset + the alarm's own volume index, for the in-place ring. */
void rg_clock_alarm_prefs(int *preset, int *volume);

/* ---- pure next-alarm math (testable) -----------------------------------
 * Epoch (unix seconds) of the soonest future HH:MM:00 among `mins`, treating
 * each as a daily alarm; 0 if none. Handles the midnight wrap. */
time_t rg_alarm_next_epoch(const uint16_t *mins, int count, time_t now);

/* ---- wake-cause decision (pure) ---------------------------------------- */
typedef enum { RG_WAKE_NONE = 0, RG_WAKE_BUTTON, RG_WAKE_ALARM } rg_wake_cause_t;
/* alraf        = RTC Alarm A flag was set at wake (authoritative on STANDBY)
 * wkup_button  = PWR wakeup-pin flag was set (the power button)
 * cache_due    = the resident next-alarm epoch is now in the past (STOP2, where
 *                the alarm ISR already cleared ALRAF before we could read it) */
rg_wake_cause_t rg_alarm_wake_decide(bool alraf, bool wkup_button, bool cache_due);

/* ---- synth tone presets (pure tables) ---------------------------------- */
enum { RG_TONE_BEEP = 0, RG_TONE_BEEP2, RG_TONE_CHIRP, RG_TONE_SIREN, RG_TONE_COUNT };
/* cfg token <-> preset id. from_token returns -1 for a non-preset string
 * (e.g. an SD sound file name); token() returns the ASCII cfg token. */
int         rg_tone_preset_from_token(const char *tok);
const char *rg_tone_preset_token(int preset);
/* Instantaneous tone shape for `preset` at time now_ms: full square-wave period
 * in samples (0 = silent), and *on = whether the gate is open right now. Pure. */
int         rg_alarm_tone_step(int preset, uint32_t now_ms, bool *on);

/* ================= firmware-only below (needs HAL) ================= */
#ifndef RG_ALARM_HOST

/* Recompute the next-alarm epoch from the clock config + current RTC time and
 * mirror it to RTC backup DR1 (survives STANDBY). Call at boot after
 * GW_RTC_RestoreIfLost() and whenever the clock config is saved. */
void   rg_alarm_cache_refresh(void);
/* Restore the mirrored epoch from DR1 into the resident cache (STANDBY wake, the
 * config on SD may not be re-read yet). */
void   rg_alarm_cache_load_backup(void);
time_t rg_alarm_cache_next_epoch(void);
/* True once the current RTC time has reached the cached next-alarm epoch. */
bool   rg_alarm_cache_due(void);
/* Push the cache to the NEXT occurrence (call right after a ring is dismissed so
 * the same minute does not re-fire). */
void   rg_alarm_cache_advance(void);

/* Program RTC Alarm A to the cached next-alarm HH:MM (date+seconds masked =
 * daily), or deactivate it if nothing is armed. Idempotent: deactivates and
 * clears any stale flag first, so it is safe to call on every sleep entry. */
void rg_alarm_arm_rtc(void);
/* Read the RTC Alarm A flag without clearing it (boot wake-cause check). */
bool rg_alarm_rtc_flag(void);

/* Feed one freed SAI half with the tone for `preset` at volume index `vol`;
 * manages audio_start/stop on the ringing transition. Shared by the clock and
 * the in-place ring so there is ONE tone generator. */
void rg_alarm_tone_feed(uint32_t now, bool ringing, int preset, int vol);

/* Blocking in-place ring for the non-clock states (in-game / music / video):
 * stops audio, shows a pulsing banner + beep until dismissed (A/B/POWER), then
 * restores audio and advances the cache. No sleep, no savestate — control
 * returns exactly where it left off. */
void rg_alarm_ring_inplace(void);

/* 1Hz-gated convenience for the app loops (in-game, music, video): at most once
 * a second, if an alarm is due, run the in-place ring. Returns true if it rang
 * (the caller may want to force a repaint). Call once per loop iteration. */
bool rg_alarm_poll(void);

#endif /* !RG_ALARM_HOST */

#ifdef __cplusplus
}
#endif
