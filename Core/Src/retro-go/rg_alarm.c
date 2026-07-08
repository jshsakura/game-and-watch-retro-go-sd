/* All-state alarm: resident next-alarm cache + shared ring pieces.  See
 * Core/Inc/retro-go/rg_alarm.h for the why.  The top half is pure (host-tested);
 * the HAL half is compiled out with -DRG_ALARM_HOST. */

#include "rg_alarm.h"
#include <time.h>
#include <string.h>
#include <strings.h>   /* strcasecmp — preset tokens match case-insensitively */

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 48000        /* host build: gw_audio.h not included */
#endif

/* ---------------------------------------------------------------- pure ---- */

time_t rg_alarm_next_epoch(const uint16_t *mins, int count, time_t now)
{
    if (count <= 0) return 0;
    struct tm tm;
    localtime_r(&now, &tm);
    int now_mod = tm.tm_hour * 60 + tm.tm_min;
    int best = 24 * 60 + 1;
    for (int i = 0; i < count; i++) {
        int d = (int)mins[i] - now_mod;
        if (d <= 0) d += 24 * 60;       /* passed (or exactly now) -> next day */
        if (d < best) best = d;
    }
    /* midnight of the current day, then step forward to the target HH:MM:00 */
    time_t midnight = now - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    return midnight + (time_t)(now_mod + best) * 60;
}

rg_wake_cause_t rg_alarm_wake_decide(bool alraf, bool wkup_button, bool cache_due)
{
    if (alraf)       return RG_WAKE_ALARM;   /* RTC alarm flag: authoritative (STANDBY) */
    if (wkup_button) return RG_WAKE_BUTTON;  /* power button */
    if (cache_due)   return RG_WAKE_ALARM;   /* STOP2: ISR cleared ALRAF, cache says due */
    return RG_WAKE_NONE;
}

/* Preset shapes.  hz2 != 0 => siren (alternate hz/hz2, no gap); sweep => chirp
 * (rising within the on window).  A few bytes each — one generator reads them. */
typedef struct { uint16_t hz, hz2, on_ms, gap_ms; uint8_t sweep; } rg_tone_param_t;
static const rg_tone_param_t RG_TONE[RG_TONE_COUNT] = {
    /* BEEP  */ {  880,    0, 250, 250, 0 },
    /* BEEP2 */ { 1100,    0,  90,  90, 0 },
    /* CHIRP */ {  600,    0, 400, 200, 1 },
    /* SIREN */ {  700, 1000, 300,   0, 0 },
};
static const char *const RG_TONE_TOK[RG_TONE_COUNT] = { "beep", "beep2", "chirp", "siren" };

int rg_tone_preset_from_token(const char *tok)
{
    if (tok)
        for (int i = 0; i < RG_TONE_COUNT; i++)
            if (strcasecmp(tok, RG_TONE_TOK[i]) == 0) return i;   /* "Beep"/"beep" both match */
    return -1;
}

const char *rg_tone_preset_token(int preset)
{
    if (preset < 0 || preset >= RG_TONE_COUNT) preset = RG_TONE_BEEP;
    return RG_TONE_TOK[preset];
}

int rg_alarm_tone_step(int preset, uint32_t now_ms, bool *on)
{
    if (preset < 0 || preset >= RG_TONE_COUNT) preset = RG_TONE_BEEP;
    const rg_tone_param_t *p = &RG_TONE[preset];
    int hz = p->hz;
    bool gate;
    if (p->hz2) {                                  /* siren: two tones, continuous */
        gate = true;
        hz = ((now_ms / 300) & 1) ? p->hz2 : p->hz;
    } else {
        uint32_t cycle = (uint32_t)p->on_ms + p->gap_ms;
        uint32_t ph = cycle ? (now_ms % cycle) : 0;
        gate = (p->gap_ms == 0) || (ph < p->on_ms);
        if (p->sweep && gate)                      /* chirp: hz..2hz across on window */
            hz = p->hz + (int)((uint32_t)p->hz * ph / (p->on_ms ? p->on_ms : 1));
    }
    if (on) *on = gate && hz > 0;
    return hz > 0 ? (AUDIO_SAMPLE_RATE / hz) : 0;
}

/* =============================================================== firmware == */
#ifndef RG_ALARM_HOST

#include "main.h"
#include "stm32h7xx_hal.h"
#include "rg_rtc.h"
#undef AUDIO_SAMPLE_RATE          /* let gw_audio.h own the real definition */
#include "gw_audio.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "gw_lcd.h"
#include "odroid_overlay.h"
#include "common.h"        /* volume_tbl */

#define RG_ALARM_BKP_REG   RTC_BKP_DR1   /* DR0/29/30 taken; DR1 is free */

/* The whole resident footprint of this feature: one epoch + a little tone state.
 * Lands in .bss (DTCM) — kept to a handful of ints on purpose. */
static time_t   s_next_epoch;
static bool     s_tone_on;
static uint32_t s_tone_phase;
static uint32_t s_tone_dma_mark;

/* ---- cache ------------------------------------------------------------- */

static void bkup_write_epoch(time_t e)
{
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&hrtc, RG_ALARM_BKP_REG, (uint32_t)e);
}

void rg_alarm_cache_refresh(void)
{
    rg_alarm_query_t q;
    rg_clock_query_alarms(&q);                 /* reuses the clock's cfg parser */
    time_t now = GW_GetUnixTime();
    s_next_epoch = q.dnd ? 0 : rg_alarm_next_epoch(q.mins, q.count, now);
    bkup_write_epoch(s_next_epoch);
}

void rg_alarm_cache_load_backup(void)
{
    s_next_epoch = (time_t)(int32_t)HAL_RTCEx_BKUPRead(&hrtc, RG_ALARM_BKP_REG);
}

time_t rg_alarm_cache_next_epoch(void) { return s_next_epoch; }

bool rg_alarm_cache_due(void)
{
    return s_next_epoch != 0 && GW_GetUnixTime() >= s_next_epoch;
}

void rg_alarm_cache_advance(void)
{
    /* recompute from the config so a just-fired daily alarm rolls to tomorrow */
    rg_alarm_cache_refresh();
}

/* ---- deep-sleep RTC Alarm A ------------------------------------------- */

bool rg_alarm_rtc_flag(void)
{
    return __HAL_RTC_ALARM_GET_FLAG(&hrtc, RTC_FLAG_ALRAF) != 0;
}

void rg_alarm_arm_rtc(void)
{
    HAL_PWR_EnableBkUpAccess();
    /* always deactivate + clear first: never leave a stale ALRAF behind */
    HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A);
    __HAL_RTC_ALARM_CLEAR_FLAG(&hrtc, RTC_FLAG_ALRAF);

    if (s_next_epoch == 0) return;             /* nothing enabled: stay disarmed */

    time_t e = s_next_epoch;
    struct tm tm;
    localtime_r(&e, &tm);

    RTC_AlarmTypeDef a = {0};
    a.AlarmTime.Hours   = tm.tm_hour;
    a.AlarmTime.Minutes = tm.tm_min;
    a.AlarmTime.Seconds = 0;
    a.AlarmMask         = RTC_ALARMMASK_DATEWEEKDAY;   /* match H:M:S, any date = daily */
    a.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
    a.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
    a.AlarmDateWeekDay  = 1;
    a.Alarm             = RTC_ALARM_A;
    HAL_RTC_SetAlarm_IT(&hrtc, &a, RTC_FORMAT_BIN);
}

/* ---- shared tone feed -------------------------------------------------- */

void rg_alarm_tone_feed(uint32_t now, bool ringing, int preset, int vol)
{
    if (!ringing) {
        if (s_tone_on) { audio_stop_playing(); s_tone_on = false; }
        return;
    }
    if (!s_tone_on) {
        audio_start_playing(AUDIO_BUFFER_LENGTH);
        s_tone_on = true; s_tone_phase = 0; s_tone_dma_mark = dma_counter - 1;
    }
    if (dma_counter == s_tone_dma_mark) return;   /* fill each freed half exactly once */
    s_tone_dma_mark = dma_counter;

    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    if (vol < 0) vol = 0;
    if (vol > ODROID_AUDIO_VOLUME_MAX) vol = ODROID_AUDIO_VOLUME_MAX;
    int amp = (16000 * volume_tbl[vol]) >> 8;
    bool on = false;
    int period = rg_alarm_tone_step(preset, now, &on);
    int half = period / 2;
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (on && amp && period) {
            s = (s_tone_phase < (uint32_t)half) ? (int16_t)amp : (int16_t)-amp;
            if (++s_tone_phase >= (uint32_t)period) s_tone_phase = 0;
        }
        buf[i] = s;
    }
}

/* ---- in-place ring (in-game / music / video) --------------------------- */

/* Minimal pulsing banner — no clip needed (odroid_overlay_draw_* clip to the
 * LCD), no background port.  ASCII text = zero new i18n strings. */
#define RING_BANNER "* ALARM *"
#define RING_MS     60000u        /* auto-dismiss after 60s if untouched */

static void ring_draw(uint32_t now)
{
    uint16_t *fb = (uint16_t *)lcd_get_active_buffer();
    bool bright = ((now / 250) & 1);
    uint16_t bg  = bright ? 0xF800 : 0x2000;   /* pulsing red band */
    uint16_t ink = 0xFFFF;
    int bh = 44, by = (GW_LCD_HEIGHT - bh) / 2;
    (void)fb;
    odroid_overlay_draw_fill_rect(0, by, GW_LCD_WIDTH, bh, bg);
    int fw = odroid_overlay_get_font_width();
    int tw = (int)strlen(RING_BANNER) * fw;
    odroid_overlay_draw_text((GW_LCD_WIDTH - tw) / 2, by + 16, tw, RING_BANNER, ink, bg);
    lcd_swap();
}

void rg_alarm_ring_inplace(void)
{
    int preset = RG_TONE_BEEP, vol = 6;
    rg_clock_alarm_prefs(&preset, &vol);

    audio_stop_playing();                 /* silence the emulator/player audio */

    odroid_gamepad_state_t k, prev;
    odroid_input_read_gamepad(&prev);     /* swallow the key that may be held */
    uint32_t start = HAL_GetTick(), last_draw = 0;

    while (true) {
        wdog_refresh();
        uint32_t now = HAL_GetTick();
        odroid_input_read_gamepad(&k);
        bool press = (k.values[ODROID_INPUT_A]     && !prev.values[ODROID_INPUT_A]) ||
                     (k.values[ODROID_INPUT_B]     && !prev.values[ODROID_INPUT_B]) ||
                     (k.values[ODROID_INPUT_POWER] && !prev.values[ODROID_INPUT_POWER]);
        if (press || (now - start) >= RING_MS) break;

        rg_alarm_tone_feed(now, true, preset, vol);
        if (now - last_draw >= 60) { ring_draw(now); last_draw = now; }
        prev = k;
        HAL_Delay(8);
    }

    rg_alarm_tone_feed(0, false, preset, vol);   /* stop the SAI */
    rg_alarm_cache_advance();                     /* roll to the next occurrence */

    /* hand a running SAI back to the emulator/player, same as the STOP2 wake path */
    audio_start_playing_full_length(audio_get_buffer_full_length());
}

bool rg_alarm_poll(void)
{
    static uint32_t last_s;
    uint32_t s = HAL_GetTick() / 1000;
    if (s == last_s) return false;            /* at most once a second */
    last_s = s;
    if (!rg_alarm_cache_due()) return false;
    rg_alarm_ring_inplace();
    return true;
}

#endif /* !RG_ALARM_HOST */
