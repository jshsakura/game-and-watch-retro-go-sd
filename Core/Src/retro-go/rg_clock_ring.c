/* Resident half of the Clock app — see Core/Src/retro-go/rg_clock.c (the
 * OVERLAY half: rendering, settings menu, editors, linked into
 * .overlay_clock / RAM_EMU) for the rest of the picture.
 *
 * What lives here and why: the main loop, all persisted/runtime state
 * (rg_clock_state.h), config I/O, runner ticking, alarm-fire timing, and the
 * ring/MP3 dispatch. ALL of it must survive an MP3 alarm sound, which stages
 * the Music-overlay decoder over the ENTIRE RAM_EMU region (the same address
 * .overlay_clock loads at) — so anything that needs to stay valid across that
 * moment cannot itself be .overlay_clock code or data. This file's job is to
 * be exactly that survivable core, and to re-stage .overlay_clock afterward
 * before calling back into it.
 *
 * ★ Never call an .overlay_clock function (clock_overlay_frame,
 * clock_settings_menu, clock_gif_*, clock_album_*) while s_ring_mp3 is true —
 * that memory holds Music-decoder bytes at that point, not clock UI code. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "rg_rtc.h"
#include "gui.h"
#include "odroid_overlay.h"
#include "odroid_input.h"
#include "odroid_audio.h"
#include "common.h"    /* volume_tbl */
#include "rg_storage.h"
#include "rg_clock.h"
#include "rg_clock_state.h"
#include "rg_alarm.h"
#include "rg_clock_gif.h"
#include "rg_clock_album.h"
#include "rg_clock_alarm_mp3.h"

extern void rg_emulators_reset_all_lists(void);

/* ---- namespaced POSIX I/O for .overlay_clock ---------------------------
 * rg_clock_gif.c/gifdec.c/rg_clock_album.c use raw open()/read()/close() (a
 * GIF/photo decoder's natural fd-based model), unlike every other overlay's
 * file access (Music uses buffered fopen/fread/fclose exclusively). Since
 * every overlay links at the same RAM_EMU address, an undefined `close`/`read`
 * in .overlay_clock's own object files does NOT fail the link -- it silently
 * binds to whichever OTHER core's overlay happens to define a symbol by that
 * name (nes_fceu owns `close`, msx owns `read`, for their own unrelated
 * reasons), and calling that address once .overlay_clock is loaded there
 * instead reads garbage (scripts/check_core_symbol_aliases.py catches
 * exactly this class of bug). Makefile.common's clock_redefines pass renames
 * .overlay_clock's own references to clock__close/clock__read so they can't
 * alias; these two resident wrappers (NOT subject to that rename, since
 * build/core/ is exempt) are what they actually resolve to -- plain pass-
 * throughs to the real, resident close()/read(). */
int clock__close(int fd) { return close(fd); }
int clock__read(int fd, void *buf, size_t count) { return read(fd, buf, count); }

/* ---- persisted / runtime state (defined here, extern via rg_clock_state.h) */

int      s_theme;
int      s_face_override = -1;
bool     s_hour24;
bool     s_dnd;
int      s_anim;
int      s_scene;
int8_t   s_alarm_volume = 6;
int8_t   s_beep_preset = RG_TONE_BEEP;
alarm_t  s_alarms[MAX_ALARMS];
int      s_alarm_count;
int      s_last_fired_min = -1;

#if CLOCK_SD_MEDIA
char     s_bgfile[32]   = "";
char     s_alarmsnd[32] = "";
bool     s_album_used = false;
int      s_photo_speed = 1;
uint32_t s_photo_next   = 0;
uint32_t s_fade_start   = 0;
bool     s_fade_swapped = false;
static const uint32_t PHOTO_HOLD_TBL[3] = { 15000, 8000, 4000 };
#define PHOTO_HOLD_MS (PHOTO_HOLD_TBL[(s_photo_speed >= 0 && s_photo_speed < 3) ? s_photo_speed : 1])
#endif

runner_t s_timer = { RUN_STOPPED, 5*60*1000, 0, 0 };
runner_t s_watch = { RUN_STOPPED, 0, 0, 0 };
int      s_pomo_work_min = 25, s_pomo_break_min = 5, s_pomo_cycles = 0;
bool     s_pomo_on_break = false;
runner_t s_pomo = { RUN_STOPPED, 25*60*1000, 0, 0 };
uint32_t s_flash_until = 0;

/* ---- config (/clock/clock.cfg) ----------------------------------------- */

#define CLOCK_CFG_PATH    "/clock/clock.cfg"
#define CLOCK_CFG_LEGACY  "/clock.cfg"

static const char *const BEEP_LABELS[RG_TONE_COUNT] = { "Beep", "Beep2", "Chirp", "Siren" };
#define BEEP_LABEL(p) BEEP_LABELS[((p) >= 0 && (p) < RG_TONE_COUNT) ? (p) : 0]

/* THEME_COUNT lives with the theme table in the overlay file; config load
 * only needs a generous upper bound to reject corrupt values before the
 * overlay ever sees them, so it doesn't need the table itself. */
#define CLOCK_THEME_COUNT_MAX 32
#define CLOCK_FACE_LAST_MAX   16

void clock_config_load(void)
{
    s_theme = 0; s_face_override = -1;
    s_hour24 = false; s_dnd = false; s_anim = 0; s_scene = 0;

    s_alarm_volume = 6;
    s_beep_preset = RG_TONE_BEEP;
    s_alarm_count = 0;
#if CLOCK_SD_MEDIA
    s_photo_speed = 1;
    s_bgfile[0] = 0; s_alarmsnd[0] = 0;
#endif
    FILE *f = fopen(CLOCK_CFG_PATH, "r");
    if (!f) f = fopen(CLOCK_CFG_LEGACY, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v, en = 1;
        if (sscanf(line, "theme=%d", &v) == 1) { if (v >= 0 && v < CLOCK_THEME_COUNT_MAX) s_theme = v; }
        else if (sscanf(line, "face=%d", &v) == 1) { if (v >= -1 && v <= CLOCK_FACE_LAST_MAX) s_face_override = v; }
        else if (sscanf(line, "hour24=%d", &v) == 1) s_hour24 = v != 0;
        else if (sscanf(line, "dnd=%d", &v) == 1) s_dnd = v != 0;
        else if (sscanf(line, "anim=%d", &v) == 1) {
            if (v == 1) v = 0;
#if !CLOCK_SD_MEDIA
            if (v == ANIM_GIF || v == ANIM_PHOTO) v = 0;
#endif
            if (v >= 0 && v < ANIM_COUNT) s_anim = v;
        }
        else if (sscanf(line, "scene=%d", &v) == 1) { if (v >= 0) s_scene = v; }
        else if (sscanf(line, "alarmvol=%d", &v) == 1) { if (v >= 0 && v <= ODROID_AUDIO_VOLUME_MAX) s_alarm_volume = v; }
        else if (strncmp(line, "alarmsnd=", 9) == 0) { char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            int p = rg_tone_preset_from_token(line + 9);
            if (p >= 0) s_beep_preset = p;
#if CLOCK_SD_MEDIA
            if (strlen(line + 9) < sizeof s_alarmsnd) snprintf(s_alarmsnd, sizeof s_alarmsnd, "%s", line + 9);
#endif
        }
#if CLOCK_SD_MEDIA
        else if (sscanf(line, "photospeed=%d", &v) == 1) { if (v >= 0 && v < 3) s_photo_speed = v; }
        else if (strncmp(line, "bgfile=", 7) == 0)   { char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            if (strlen(line + 7) < sizeof s_bgfile)   snprintf(s_bgfile,   sizeof s_bgfile,   "%s", line + 7); }
#endif
        else if (sscanf(line, "alarm=%d,%d", &v, &en) >= 1 && s_alarm_count < MAX_ALARMS) {
            int hr = v / 100, mn = v % 100;
            if (v < 0 || hr > 23 || mn > 59) continue;
            s_alarms[s_alarm_count].hour = hr;
            s_alarms[s_alarm_count].min  = mn;
            s_alarms[s_alarm_count].enabled = en ? 1 : 0;
            s_alarm_count++;
        }
    }
    fclose(f);
}

void clock_config_save(void)
{
    rg_storage_mkdir("/clock");
    FILE *f = fopen(CLOCK_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", s_theme);
    fprintf(f, "face=%d\n", s_face_override);
    fprintf(f, "hour24=%d\n", s_hour24 ? 1 : 0);
    fprintf(f, "dnd=%d\n", s_dnd ? 1 : 0);
    fprintf(f, "anim=%d\n", s_anim);
    fprintf(f, "scene=%d\n", s_scene);
    fprintf(f, "alarmvol=%d\n", s_alarm_volume);
#if CLOCK_SD_MEDIA
    fprintf(f, "photospeed=%d\n", s_photo_speed);
    if (s_bgfile[0])   fprintf(f, "bgfile=%s\n", s_bgfile);
    if (s_alarmsnd[0]) fprintf(f, "alarmsnd=%s\n", s_alarmsnd);
#else
    fprintf(f, "alarmsnd=%s\n", BEEP_LABEL(s_beep_preset));
#endif
    for (int i = 0; i < s_alarm_count; i++)
        fprintf(f, "alarm=%02d%02d,%d\n", s_alarms[i].hour, s_alarms[i].min,
                s_alarms[i].enabled ? 1 : 0);
    fclose(f);

    rg_alarm_cache_refresh();
}

/* ---- exports for the resident all-state alarm cache (rg_alarm.c) ------- */

void rg_clock_query_alarms(rg_alarm_query_t *out)
{
    clock_config_load();
    out->count = 0;
    out->dnd = s_dnd;
    for (int i = 0; i < s_alarm_count && out->count < RG_ALARM_MAX; i++)
        if (s_alarms[i].enabled)
            out->mins[out->count++] = (uint16_t)(s_alarms[i].hour * 60 + s_alarms[i].min);
}

void rg_clock_alarm_prefs(int *preset, int *volume)
{
    clock_config_load();
    int p = s_beep_preset;
#if CLOCK_SD_MEDIA
    int t = rg_tone_preset_from_token(s_alarmsnd);
    if (t >= 0) p = t;
#endif
    if (preset) *preset = p;
    if (volume) *volume = s_alarm_volume;
}

/* ---- runners ------------------------------------------------------------ */

static void tick_countdown(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING) return;
    uint32_t dt = now - r->last_tick; r->last_tick = now;
    if (dt >= r->remaining_ms) { r->remaining_ms = 0; r->state = RUN_STOPPED; }
    else r->remaining_ms -= dt;
}
void tick_countup(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING) return;
    r->elapsed_ms += now - r->last_tick; r->last_tick = now;
}
static void runner_toggle(runner_t *r, uint32_t now)
{
    if (r->state == RUN_RUNNING) r->state = RUN_PAUSED;
    else { r->state = RUN_RUNNING; r->last_tick = now; }
}

void update_pomodoro(uint32_t now)
{
    tick_countdown(&s_pomo, now);
    if (s_pomo.state == RUN_STOPPED && s_pomo.remaining_ms == 0) {
        s_flash_until = now + 800; s_pomo_on_break = !s_pomo_on_break;
        if (!s_pomo_on_break) s_pomo_cycles++;
        s_pomo.remaining_ms = (s_pomo_on_break ? s_pomo_break_min : s_pomo_work_min) * 60u*1000u;
        s_pomo.state = RUN_RUNNING; s_pomo.last_tick = now;
    }
}

void update_timer(uint32_t now)
{
    tick_countdown(&s_timer, now);
    if (s_timer.state == RUN_STOPPED && s_timer.remaining_ms == 0 && s_flash_until < now)
        s_flash_until = now + 800;
}

static bool pressed(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, int key)
{ return k->values[key] && !p->values[key]; }

void input_pomodoro(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) {
        if (s_pomo.state == RUN_STOPPED) { s_pomo_on_break = false; s_pomo.remaining_ms = s_pomo_work_min*60u*1000u; }
        runner_toggle(&s_pomo, now);
    }
    if (pressed(k, p, ODROID_INPUT_B)) {
        s_pomo.state = RUN_STOPPED; s_pomo_on_break = false; s_pomo_cycles = 0;
        s_pomo.remaining_ms = s_pomo_work_min*60u*1000u;
    }
    if (s_pomo.state != RUN_RUNNING) {
        if (pressed(k, p, ODROID_INPUT_UP)   && s_pomo_work_min < 90) s_pomo.remaining_ms = (++s_pomo_work_min)*60u*1000u;
        if (pressed(k, p, ODROID_INPUT_DOWN) && s_pomo_work_min > 1)  s_pomo.remaining_ms = (--s_pomo_work_min)*60u*1000u;
    }
}

void input_timer(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_timer, now);
    if (pressed(k, p, ODROID_INPUT_B)) { s_timer.state = RUN_STOPPED; s_timer.remaining_ms = 5*60*1000; }
    if (s_timer.state != RUN_RUNNING) {
        if (pressed(k, p, ODROID_INPUT_UP)) s_timer.remaining_ms += 60*1000;
        if (pressed(k, p, ODROID_INPUT_DOWN) && s_timer.remaining_ms >= 60*1000) s_timer.remaining_ms -= 60*1000;
    }
}

void input_stopwatch(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_watch, now);
    if (pressed(k, p, ODROID_INPUT_B)) { s_watch.state = RUN_STOPPED; s_watch.elapsed_ms = 0; }
}

/* ---- alarm firing (while awake) ----------------------------------------- */

static bool alarm_should_fire(int hh, int mm)
{
    int mod = hh * 60 + mm;
    if (mod != s_last_fired_min) s_last_fired_min = -1;
    if (s_dnd) return false;
    if (mod == s_last_fired_min) return false;
    for (int i = 0; i < s_alarm_count; i++)
        if (s_alarms[i].enabled && s_alarms[i].hour == hh && s_alarms[i].min == mm) {
            s_last_fired_min = mod; return true;
        }
    return false;
}

static bool alarm_fired_in_window(int from_mod, int to_mod)
{
    if (s_dnd) return false;
    int day = 24 * 60, span = (to_mod - from_mod + day) % day;
    for (int i = 0; i < s_alarm_count; i++) {
        if (!s_alarms[i].enabled) continue;
        int d = ((s_alarms[i].hour * 60 + s_alarms[i].min) - from_mod + day) % day;
        if (d > 0 && d < span) return true;
    }
    return false;
}

/* Minutes-from-now to the soonest enabled alarm; -1 if none. Fills *idx.
 * Non-static: render_clock() (rg_clock.c, overlay) calls this for the
 * "next alarm" line. */
int next_alarm(int now_h, int now_m, int *idx)
{
    int best = 100000, bi = -1, now = now_h * 60 + now_m;
    for (int i = 0; i < s_alarm_count; i++) {
        if (!s_alarms[i].enabled) continue;
        int t = s_alarms[i].hour * 60 + s_alarms[i].min;
        int d = t - now; if (d <= 0) d += 24 * 60;   /* next occurrence */
        if (d < best) { best = d; bi = i; }
    }
    if (idx) *idx = bi;
    return bi < 0 ? -1 : best;
}

static bool clock_should_idle_sleep(bool ringing, clock_mode_t mode, bool timeout_expired)
{
    return !ringing && mode == MODE_CLOCK && timeout_expired;
}

/* Trivial, state-free -- duplicated from rg_clock.c's copy rather than
 * exposed across the resident/overlay boundary for one helper. */
static void fb_fill_screen(uint16_t *fb, uint16_t c)
{
    uint32_t c2 = (uint32_t)c | ((uint32_t)c << 16);
    uint32_t *p = (uint32_t *)fb;
    for (int i = 0; i < (GW_LCD_WIDTH * GW_LCD_HEIGHT) / 2; i++) p[i] = c2;
}

/* ---- .overlay_clock staging --------------------------------------------- */

#define CLOCK_OVERLAY_PATH "/cores/clock.bin"

static void clock_stage_overlay(void)
{
    wdog_refresh();
    odroid_overlay_cache_file_in_ram(CLOCK_OVERLAY_PATH, (uint8_t *)&__RAM_EMU_START__);
    memset(&_OVERLAY_CLOCK_BSS_START, 0, (size_t)&_OVERLAY_CLOCK_BSS_SIZE);
    SCB_CleanDCache_by_Addr((uint32_t *)&__RAM_EMU_START__, (size_t)&_OVERLAY_CLOCK_SIZE);
    /* Every core that calls ram_malloc() for its own buffers must point
     * ram_start past its own static overlay+bss first (see main_gwenesis.c's
     * ram_start = &_OVERLAY_MD_BSS_END / commit 8e47e219's SegaCD fix for the
     * same class of bug) -- .overlay_clock's GIF/photo arena (clock_overlay_arena
     * below) depends on this being current. */
    ram_start = (uint32_t)&_OVERLAY_CLOCK_BSS_END;
    wdog_refresh();
}

uint8_t *clock_overlay_arena(size_t *out_bytes)
{
    if (out_bytes)
        *out_bytes = (size_t)((uint8_t *)&__RAM_EMU_END__ - (uint8_t *)&_OVERLAY_CLOCK_BSS_END);
    return (uint8_t *)&_OVERLAY_CLOCK_BSS_END;
}

/* ---- alarm tone / ring dispatch ------------------------------------------
 *
 * ★ clock_alarm_mp3_start() stages the Music-overlay decoder over the WHOLE
 * RAM_EMU region -- the same address .overlay_clock is loaded at. From the
 * moment it succeeds until clock_alarm_mp3_stop() + clock_stage_overlay() run
 * again, .overlay_clock's own code+data does not exist in memory: NOTHING in
 * this file may call an overlay function (clock_overlay_frame,
 * clock_settings_menu, clock_gif_*, clock_album_*) until re-staged. */

static bool s_tone_on = false;
static bool s_ring_mp3 = false;

/* non-static: clock_menu_repaint() (rg_clock.c, overlay) calls this every
 * frame a settings/alarm dialog is open, to force the tone off behind a modal. */
void tone_feed(uint32_t now, bool ringing)
{
    s_tone_on = ringing;
    rg_alarm_tone_feed(now, ringing, s_beep_preset, s_alarm_volume);
}

#if CLOCK_SD_MEDIA
static void clock_bg_suspend(void)
{
    if (s_anim == ANIM_GIF)   clock_gif_free();
    if (s_anim == ANIM_PHOTO) clock_album_close();
}
static void clock_bg_restore(void)
{
    if (s_anim == ANIM_GIF)   clock_gif_load();
    if (s_anim == ANIM_PHOTO && clock_album_open()) s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS;
}
#endif

static void ring_audio(uint32_t now, bool ringing)
{
#if CLOCK_SD_MEDIA
    if (!ringing) {
        if (s_ring_mp3) {
            clock_alarm_mp3_stop();
            /* RAM_EMU held the Music decoder while mp3 played -- .overlay_clock
             * must be re-staged before ANYTHING below touches it. */
            clock_stage_overlay();
            clock_bg_restore();
            s_ring_mp3 = false;
        }
        tone_feed(now, false);
        return;
    }
    if (!s_ring_mp3 && !s_tone_on) {
        if (rg_tone_preset_from_token(s_alarmsnd) < 0) {
            clock_alarm_mp3_set_file(s_alarmsnd);
            if (clock_alarm_mp3_available()) {
                clock_bg_suspend();
                s_album_used = true;
                if (clock_alarm_mp3_start()) s_ring_mp3 = true;
                else { clock_stage_overlay(); clock_bg_restore(); }
            }
        }
    }
    if (s_ring_mp3) clock_alarm_mp3_service(volume_tbl[s_alarm_volume]);
    else            tone_feed(now, true);
#else
    tone_feed(now, ringing);
#endif
}

/* ---- boot-time hooks (called from rg_main.c whether or not the clock app
 * is ever opened this session) -------------------------------------------- */

void clock_ensure_dirs(void)
{
    rg_storage_mkdir("/clock");
#if CLOCK_SD_MEDIA
    rg_storage_mkdir("/clock/gif");
    rg_storage_mkdir("/clock/alarm");
    rg_storage_mkdir("/clock/album");
#endif
}

void clock_gif_reserve(void)
{
    /* No-op: the decode arena is borrowed from clock_overlay_arena() at load
     * time now (see rg_clock_gif.c). Kept so the boot call site is undisturbed. */
}

/* ---- main loop ----------------------------------------------------------- */

#define SNOOZE_MS (5u * 60u * 1000u)
#define ALARM_RING_MS 60000u
static uint32_t s_snooze_tick = 0;

void rg_clock_show(void)
{
    clock_mode_t mode = MODE_CLOCK;
    odroid_gamepad_state_t k, prev = {0};
    uint32_t alarm_ring_until = 0;
    uint32_t last_input = HAL_GetTick();
    bool force_dirty = true;

    clock_stage_overlay();
    clock_config_load();
    lcd_backlight_set(odroid_display_get_backlight_raw());
    clock_ensure_dirs();
    s_snooze_tick = 0;
#if CLOCK_SD_MEDIA
    s_album_used = false;
    if (s_anim == ANIM_GIF) { clock_gif_set_file(s_bgfile);
        if (clock_gif_load()) s_album_used = true; }
    if (s_anim == ANIM_PHOTO) {
        if (clock_album_open()) { s_album_used = true; s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS; }
    }
#endif
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();
        if (k.bitmask & ~prev.bitmask) last_input = now;
        int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();

        if (alarm_should_fire(hh, mm)) alarm_ring_until = now + ALARM_RING_MS;
        if (s_snooze_tick && now >= s_snooze_tick) {
            s_snooze_tick = 0;
            alarm_ring_until = now + ALARM_RING_MS;
        }
        bool ringing = alarm_ring_until > now;

        if (ringing && (k.bitmask & ~prev.bitmask)) {
            alarm_ring_until = 0;
            ring_audio(now, false);
            if (pressed(&k, &prev, ODROID_INPUT_A))
                s_snooze_tick = now + SNOOZE_MS;
            else
                s_snooze_tick = 0;
            prev = k; force_dirty = true;
            HAL_Delay(40);
            continue;
        }
        ring_audio(now, ringing);

        /* While the MP3 decoder occupies RAM_EMU, .overlay_clock does not
         * exist in memory -- skip every overlay call (rendering, menu,
         * gif/album) and just service audio + poll input until dismissed. */
        if (s_ring_mp3) {
            prev = k;
            HAL_Delay(8);
            continue;
        }

        bool idle_sleep = clock_should_idle_sleep(ringing, mode,
                              odroid_idle_timeout_expired((now - last_input) / 1000u));

        if (pressed(&k, &prev, ODROID_INPUT_POWER) || idle_sleep) {
            ring_audio(now, false);
#if CLOCK_SD_MEDIA
            bool had_gif = (s_anim == ANIM_GIF);
            if (had_gif) clock_gif_free();
#endif
            odroid_system_sleep();
#if CLOCK_SD_MEDIA
            if (had_gif) clock_gif_load();
#endif
            do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); }
            while (k.values[ODROID_INPUT_POWER]);
            odroid_input_read_gamepad(&prev);
            s_last_fired_min = -1;
            lcd_backlight_set(odroid_display_get_backlight_raw());
            last_input = HAL_GetTick();
            force_dirty = true;
            continue;
        }

        if (!ringing && pressed(&k, &prev, ODROID_INPUT_START)) {
            do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); }
            while (k.values[ODROID_INPUT_START]);
            break;
        }

        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { mode = (mode == 0) ? MODE_COUNT-1 : mode-1; force_dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { mode = (mode+1) % MODE_COUNT; force_dirty = true; }

        if (pressed(&k, &prev, ODROID_INPUT_VOLUME)) {
            int pre_mod = hh * 60 + mm;
            bool exit_req = clock_settings_menu();
            if (alarm_fired_in_window(pre_mod, GW_GetCurrentHour() * 60 + GW_GetCurrentMinute()))
                alarm_ring_until = HAL_GetTick() + ALARM_RING_MS;
            if (exit_req) break;
            last_input = HAL_GetTick();
            force_dirty = true;
        }

        switch (mode) {
        case MODE_POMODORO:  input_pomodoro(&k, &prev, now);  update_pomodoro(now); break;
        case MODE_TIMER:     input_timer(&k, &prev, now);     update_timer(now);    break;
        case MODE_STOPWATCH: input_stopwatch(&k, &prev, now); tick_countup(&s_watch, now); break;
        default: break;
        }

        clock_overlay_frame(mode, ringing, now, last_input, force_dirty);
        force_dirty = false;

        prev = k;
        uint32_t poll = ringing ? 8
#if CLOCK_SD_MEDIA
                      : (s_fade_start || s_anim == ANIM_SCENE) ? 16
                      : (s_anim == ANIM_GIF && clock_gif_ready()) ? 24
#else
                      : (s_anim == ANIM_SCENE) ? 16
#endif
                      : 40;
        HAL_Delay(poll);
    }

    ring_audio(0, false);
    lcd_backlight_set(odroid_display_get_backlight_raw());
#if CLOCK_SD_MEDIA
    clock_gif_free();
    if (s_album_used) {
        clock_album_close();
        rg_emulators_reset_all_lists();
        tab_t *cur = gui_get_current_tab();
        if (cur) gui_refresh_tab(cur);
        s_album_used = false;
    }
#endif
    /* clears both LCD buffers on the way out: the launcher's gui_redraw only
     * repaints header + status bar + list rows, never the whole frame, so
     * clock-era pixels would otherwise linger until a game forces a full
     * repaint. */
    fb_fill_screen(lcd_get_active_buffer(), 0x0000);
    lcd_swap(); lcd_sleep_while_swap_pending();
    fb_fill_screen(lcd_get_active_buffer(), 0x0000);
    clock_config_save();
}
