/* Host unit test extending Clock coverage beyond test_clock_alarm.c /
 * test_clock_gif.c: config round-trip breadth (theme/face/format/anim/scene/
 * photo-speed + legacy-path migration + out-of-range rejection + MAX_ALARMS
 * cap), alarm-list bookkeeping (delete/tie-break), the pomodoro/timer/
 * stopwatch runner state machine, the date/time editor's month/day/year
 * rollover (Feb 29, month wrap, year clamp), and the clock_should_idle_sleep()
 * guard (the clock has no power setting of its own — see rg_clock.c) — Does
 * NOT duplicate anything already covered by test_clock_alarm.c (refire/DND/
 * window/cfg-alarms/tone-DMA/basic next_alarm).
 *
 * Build + run (wired into tests/run.sh):
 *   mkdir -p /tmp/mtest
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/clock_stubs tests/test_clock_more.c -o /tmp/mtest/test_clock_more
 *   /tmp/mtest/test_clock_more
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* redirect /clock.cfg to /tmp/mtest so the test needs no root FS access
 * (identical trick to test_clock_alarm.c) */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/mtest/t%s", p + 1);
  for (char *c = b + 11; *c; c++) if (*c == '/') *c = '_';   /* flatten subdirs */
  return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock.c"
#undef fopen

/* ---- stubs (same firmware surface as test_clock_alarm.c) --------------- */
static uint32_t fake_tick = 0;
uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_Delay(uint32_t ms) { fake_tick += ms; }
void wdog_refresh(void) {}
void odroid_system_sleep(void) {}
bool clock_album_open(void) { return false; }
bool clock_album_ready(void) { return false; }
const uint16_t *clock_album_current(void) { return 0; }
void clock_album_advance(void) {}
int clock_album_count(void) { return 0; }
void clock_album_close(void) {}
void rg_emulators_reset_all_lists(void) {}
tab_t *gui_get_current_tab(void) { return 0; }
void gui_refresh_tab(tab_t *tab) { (void)tab; }
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef *h, uint8_t *b, uint16_t l) { (void)h;(void)b;(void)l; return 0; }
int HAL_SAI_DMAStop(SAI_HandleTypeDef *h) { (void)h; return 0; }
SAI_HandleTypeDef hsai_BlockA1; DMA_HandleTypeDef hdma_sai1_a;

static uint16_t fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];
uint16_t *lcd_get_active_buffer(void) { return fb; }
void lcd_swap(void) {}
void lcd_sleep_while_swap_pending(void) {}
void lcd_backlight_set(uint8_t b) { (void)b; }
/* same raw table as odroid_display.c's backlightLevels[] / brightness_update_cb —
 * the settings-menu Brightness row edits this directly via
 * odroid_display_get_backlight()/set_backlight(), same as the common PAUSE menu */
static const uint8_t backlightLevels[10] = {128,130,133,139,149,162,178,198,222,255};
static int stub_backlight_level = 6;
int odroid_display_get_backlight(void) { return stub_backlight_level; }
void odroid_display_set_backlight(int level) { stub_backlight_level = level; }
uint8_t odroid_display_get_backlight_raw(void) { return backlightLevels[stub_backlight_level]; }
/* rg_clock_show() (unused directly by these tests, but compiled) asks the
 * launcher's global idle timeout; clock_should_idle_sleep() below is tested
 * directly with an explicit timeout_expired argument, so this stub just needs
 * to link. */
bool odroid_idle_timeout_expired(uint32_t idle_seconds) { (void)idle_seconds; return false; }

/* every lang_t field is a `const char *` — point them all at one placeholder
 * so ANY string the renderer reaches for (not just the ones the alarm test
 * happened to touch) is non-NULL. The date/time editor exercises several
 * fields the alarm test never renders (s_Clock_Set_Time, s_Clock_Hint_Edit). */
static lang_t L;
static void init_lang(void)
{
    const char **p = (const char **)&L;
    for (size_t i = 0; i < sizeof(L) / sizeof(const char *); i++) p[i] = "x";
}
const lang_t *curr_lang = &L;
int i18n_draw_text_line(int x,int y,int w,const char*t,uint16_t c,uint16_t bg,int f){(void)x;(void)y;(void)w;(void)t;(void)c;(void)bg;(void)f;return 0;}
int i18n_get_text_width(const char *t){ return (int)strlen(t) * 6; }
int odroid_overlay_dialog(const char*h,odroid_dialog_choice_t*o,int s,void(*r)(void),int f){(void)h;(void)o;(void)s;(void)r;(void)f;return -1;}
/* fill_rect that actually paints into fb with a hard bounds guard: the device
 * blitter has NO clipping, so a stray off-screen rect wraps into the next row.
 * g_oob counts any out-of-bounds pixel while g_check_bounds is armed, which the
 * ring-overlay test uses to prove draw_ring_overlay stays inside 320x240. */
static int g_oob = 0, g_check_bounds = 0;
void odroid_overlay_draw_fill_rect(int x,int y,int w,int h,uint16_t c){
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            if (px < 0 || px >= GW_LCD_WIDTH || py < 0 || py >= GW_LCD_HEIGHT) {
                if (g_check_bounds) g_oob++;
                continue;
            }
            fb[py * GW_LCD_WIDTH + px] = c;
        }
}
void odroid_overlay_draw_text(int x,int y,int w,const char*t,uint16_t c,uint16_t b){(void)x;(void)y;(void)w;(void)t;(void)c;(void)b;}
void odroid_overlay_draw_logo(int x,int y,int l,uint16_t c){(void)x;(void)y;(void)l;(void)c;}
retro_logo_image *rg_get_logo(int16_t i){ (void)i; return NULL; }
void odroid_overlay_draw_battery(int p,int x,int y){(void)p;(void)x;(void)y;}
int odroid_overlay_get_font_width(void){return 8;}
int odroid_input_read_battery(void){return 50;}
int GW_GetCurrentHour(void){return 0;} int GW_GetCurrentMinute(void){return 0;}
int GW_GetCurrentSubSeconds(void){return 0;} int GW_GetCurrentMonth(void){return 1;}
int GW_GetCurrentDay(void){return 1;} int GW_GetCurrentWeekday(void){return 1;}
bool clock_gif_ready(void){return false;}
void clock_gif_blit(uint16_t*f,uint32_t n){(void)f;(void)n;}
bool clock_gif_load(void){return false;} void clock_gif_free(void){}
int clock_gif_status(void){return 1;}
const char *clock_gif_diag(void){return "";}
void clock_gif_set_file(const char *n){ (void)n; }
/* MP3-alarm engine — inert here (no alarm file, so ring_audio always beeps) */
bool clock_alarm_mp3_available(void){ return false; }
bool clock_alarm_mp3_start(void){ return false; }
void clock_alarm_mp3_service(int v){ (void)v; }
void clock_alarm_mp3_stop(void){}
bool clock_alarm_mp3_active(void){ return false; }
void clock_alarm_mp3_set_file(const char *n){ (void)n; }

/* simulated SAI DMA (only linked because rg_clock_show/tone_feed reference it;
 * never exercised by these tests) */
uint32_t audio_mute;
int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2];
dma_transfer_state_t dma_state; uint32_t dma_counter;
static uint16_t full_len = AUDIO_BUFFER_LENGTH * 2;
uint16_t audio_get_buffer_full_length(void){ return full_len; }
uint16_t audio_get_buffer_length(void){ return full_len / 2; }
uint16_t audio_get_buffer_size(void){ return audio_get_buffer_length()*2; }
int16_t *audio_get_active_buffer(void){ return &audiobuffer_dma[dma_state == DMA_TRANSFER_STATE_HF ? 0 : full_len/2]; }
int16_t *audio_get_inactive_buffer(void){ return &audiobuffer_dma[dma_state == DMA_TRANSFER_STATE_TC ? 0 : full_len/2]; }
void audio_clear_active_buffer(void){ memset(audio_get_active_buffer(),0,audio_get_buffer_size()); }
void audio_clear_inactive_buffer(void){ memset(audio_get_inactive_buffer(),0,audio_get_buffer_size()); }
void audio_clear_buffers(void){ memset(audiobuffer_dma,0,sizeof audiobuffer_dma); }
void audio_set_buffer_length(uint16_t l){ full_len = l*2; }
void audio_start_playing(uint16_t l){ audio_clear_buffers(); full_len = l*2; }
void audio_start_playing_full_length(uint16_t l){ audio_clear_buffers(); full_len = l; }
void audio_stop_playing(void){ audio_clear_buffers(); }
const uint8_t volume_tbl[ODROID_AUDIO_VOLUME_MAX + 1] = { 0, 4, 8, 15, 32, 48, 64, 96, 128, 255 };
static int stub_volume = 9;
int odroid_audio_volume_get(void) { return stub_volume; }
void odroid_audio_volume_set(int level) { stub_volume = level; }

/* ---- scripted gamepad, for driving clock_edit_time()'s interactive loop -
 * each entry is one frame; the loop's own prev/cur edge detection (see
 * pressed() in rg_clock.c) does the rest. Reads past the end of the script
 * come back idle/all-zero, which both ends the closing "drain the release"
 * do-while and prevents an unbounded loop if a scenario forgets a step. */
static odroid_gamepad_state_t g_script[32];
static int g_script_len = 0, g_script_pos = 0;
void odroid_input_read_gamepad(odroid_gamepad_state_t *s)
{
    if (g_script_pos < g_script_len) *s = g_script[g_script_pos++];
    else memset(s, 0, sizeof *s);
}
static void script_push(int key /* -1 = idle frame */)
{
    odroid_gamepad_state_t s; memset(&s, 0, sizeof s);
    if (key >= 0) { s.values[key] = 1; s.bitmask = 1u << key; }
    g_script[g_script_len++] = s;
}

/* GW_GetUnixTM/SetUnixTM: the clock's ONLY window onto the RTC (see rg_rtc.c
 * — GW_GetUnixTM/SetUnixTM clone struct tm in/out). Faking them here drives
 * clock_edit_time() without pulling in the STM32 HAL RTC registers. */
static struct tm g_init_tm;
static struct tm g_saved_tm;
static bool      g_save_called;
void GW_GetUnixTM(struct tm *tm) { *tm = g_init_tm; }
void GW_SetUnixTM(struct tm *tm) { g_saved_tm = *tm; g_save_called = true; }

/* Runs clock_edit_time() to completion against a scripted key sequence.
 * keys[0] MUST be -1 (idle): it is consumed by the pre-loop
 * odroid_input_read_gamepad(&prev) read, i.e. "nothing held when the editor
 * opened". The sequence must end on an A (confirm) or B (cancel) edge. */
static void run_edit_time(struct tm init, const int *keys, int nkeys)
{
    g_init_tm = init;
    g_save_called = false;
    g_script_len = 0; g_script_pos = 0;
    for (int i = 0; i < nkeys; i++) script_push(keys[i]);
    clock_edit_time();
}

/* ---- tests --------------------------------------------------------------*/
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

static void reset_alarms(void)
{ s_alarm_count = 0; s_dnd = false; s_last_fired_min = -1; memset(s_alarms,0,sizeof s_alarms); }
static void add_alarm(int h, int m, int en)
{ s_alarms[s_alarm_count++] = (alarm_t){ (uint8_t)h, (uint8_t)m, (uint8_t)en }; }

static void reset_cfg_fields(void)
{
    s_theme = 0; s_face_override = -1; s_hour24 = false; s_dnd = false;
    s_anim = 0; s_scene = 0; s_photo_speed = 1;
    s_alarm_volume = 6;
    s_alarm_count = 0; memset(s_alarms, 0, sizeof s_alarms);
}

/* ---- days_in_month: leap-year edge cases (Gregorian rule, century-aware) */
static void test_days_in_month(void)
{
    CHECK(days_in_month(2024, 1) == 29,  "Feb 2024 (/4 leap) has 29 days");
    CHECK(days_in_month(2023, 1) == 28,  "Feb 2023 (non-leap) has 28 days");
    CHECK(days_in_month(2000, 1) == 29,  "Feb 2000 (/400 leap) has 29 days");
    CHECK(days_in_month(1900, 1) == 28,  "Feb 1900 (/100 not /400, non-leap) has 28 days");
    CHECK(days_in_month(2024, 0) == 31,  "January always has 31 days");
    CHECK(days_in_month(2024, 3) == 30,  "April always has 30 days");
    CHECK(days_in_month(2024, 11) == 31, "December always has 31 days");
}

/* ---- clock_config_load: legacy /clock.cfg fallback (untested by
 * test_clock_alarm.c, which deliberately writes only the primary path) */
static void test_cfg_legacy_migration(void)
{
    remove(test_path(CLOCK_CFG_PATH));
    remove(test_path(CLOCK_CFG_LEGACY));

    FILE *f = fopen(test_path(CLOCK_CFG_LEGACY), "w");
    fprintf(f, "theme=4\n");
    fprintf(f, "hour24=1\n");
    fprintf(f, "alarm=0615\n");
    fclose(f);

    reset_cfg_fields();
    clock_config_load();
    CHECK(s_theme == 4,    "legacy /clock.cfg theme is read when primary is missing");
    CHECK(s_hour24 == true, "legacy /clock.cfg hour24 is read when primary is missing");
    CHECK(s_alarm_count == 1 && s_alarms[0].hour == 6 && s_alarms[0].min == 15,
          "legacy /clock.cfg alarm is read when primary is missing");

    /* once anything has ever saved, the primary path exists and must shadow
     * the legacy file from then on (the whole point of the migration) */
    s_theme = 2;
    clock_config_save();
    remove(test_path(CLOCK_CFG_LEGACY));
    FILE *f2 = fopen(test_path(CLOCK_CFG_LEGACY), "w");
    fprintf(f2, "theme=7\n");
    fclose(f2);
    reset_cfg_fields();
    clock_config_load();
    CHECK(s_theme == 2, "primary /clock/clock.cfg shadows legacy once it exists");

    remove(test_path(CLOCK_CFG_PATH));
    remove(test_path(CLOCK_CFG_LEGACY));
}

/* ---- clock_config_load/save: full field round-trip beyond alarms+dnd */
static void test_cfg_full_roundtrip(void)
{
    remove(test_path(CLOCK_CFG_PATH));
    reset_cfg_fields();
    s_theme = 5; s_face_override = FACE_LED; s_hour24 = true;  /* FACE_LED = the last (newest) face: proves the extended range persists */
    s_anim = ANIM_SCENE; s_scene = 7; s_photo_speed = 2;
    s_alarm_volume = 3;
    snprintf(s_bgfile, sizeof s_bgfile, "%s", "sunset.gif");
    snprintf(s_alarmsnd, sizeof s_alarmsnd, "%s", "chime.mp3");
    clock_config_save();

    reset_cfg_fields();
    s_bgfile[0] = 0; s_alarmsnd[0] = 0;
    clock_config_load();
    CHECK(s_theme == 5,              "theme round-trips");
    CHECK(s_face_override == FACE_LED, "face override round-trips (newest face in extended range)");
    CHECK(s_hour24 == true,          "hour24 round-trips");
    CHECK(s_anim == ANIM_SCENE,      "anim round-trips");
    CHECK(s_scene == 7,              "scene round-trips");
    CHECK(s_photo_speed == 2,        "photo speed round-trips");
    CHECK(s_alarm_volume == 3,       "alarm volume round-trips (non-default value)");
    CHECK(strcmp(s_bgfile, "sunset.gif") == 0,    "bgfile round-trips");
    CHECK(strcmp(s_alarmsnd, "chime.mp3") == 0,   "alarmsnd round-trips");

    /* an empty (default) bgfile/alarmsnd must NOT be written — "" means the
     * implicit bg.gif / alarm.mp3, and an old cfg without the key must stay
     * clean so it keeps meaning "default". */
    reset_cfg_fields();
    s_bgfile[0] = 0; s_alarmsnd[0] = 0;
    clock_config_save();
    FILE *cf = fopen(test_path(CLOCK_CFG_PATH), "r");
    char buf[512] = ""; if (cf) { size_t n = fread(buf, 1, sizeof buf - 1, cf); buf[n] = 0; fclose(cf); }
    CHECK(strstr(buf, "bgfile=")   == NULL, "empty bgfile is not persisted");
    CHECK(strstr(buf, "alarmsnd=") == NULL, "empty alarmsnd is not persisted");
}

/* ---- alarm ring overlay: forced, legible, in-bounds ----------------------*/
static void test_ring_overlay(void)
{
    const uint16_t WHITE = 0xFFFF;
    const clock_theme_t *t = TH();
    int darkened = 0, accent = 0;
    g_oob = 0; g_check_bounds = 1;
    /* sweep a beat's worth of phases; a bright "photo" background each frame */
    for (uint32_t now = 0; now < 400; now += 40) {
        for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = WHITE;
        draw_ring_overlay(fb, now, t);
        for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) {
            if (fb[i] != WHITE) darkened = 1;      /* scrim darkened the frame */
            if (fb[i] == t->alarm) accent = 1;     /* bright pulse dot present */
        }
    }
    g_check_bounds = 0;
    CHECK(g_oob == 0, "ring overlay never draws outside 320x240 (no-clip blitter safe)");
    CHECK(darkened, "ring overlay darkens the composed frame (guaranteed min contrast)");
    CHECK(accent,   "ring overlay paints bright accent pulse dots over the frame");
}

static void test_cfg_rejects_out_of_range(void)
{
    remove(test_path(CLOCK_CFG_PATH));
    FILE *f = fopen(test_path(CLOCK_CFG_PATH), "w");
    fprintf(f, "theme=99\n");   /* >= THEME_COUNT: reject */
    fprintf(f, "theme=-1\n");   /* negative: reject */
    fprintf(f, "face=99\n");    /* > FACE_LAST: reject (past the extended face range) */
    fprintf(f, "anim=1\n");     /* retired ambient value: migrates to off (0), not kept as 1 */
    fprintf(f, "alarmvol=99\n");     /* > ODROID_AUDIO_VOLUME_MAX: reject */
    fprintf(f, "alarmvol=-1\n");     /* negative: reject */
    fclose(f);

    reset_cfg_fields();
    clock_config_load();
    CHECK(s_theme == 0,         "out-of-range theme rejected, default kept");
    CHECK(s_face_override == -1, "out-of-range face rejected, default kept");
    CHECK(s_anim == 0,          "retired ambient (anim=1) migrates to off, not ANIM_SCENE");
    CHECK(s_alarm_volume == 6,  "out-of-range alarmvol rejected, default kept");

    remove(test_path(CLOCK_CFG_PATH));
}

/* ---- MAX_ALARMS cap: the file could hold more lines than the fixed array */
static void test_cfg_max_alarms_boundary(void)
{
    remove(test_path(CLOCK_CFG_PATH));
    FILE *f = fopen(test_path(CLOCK_CFG_PATH), "w");
    for (int i = 0; i < MAX_ALARMS + 1; i++)   /* one line past the cap */
        fprintf(f, "alarm=%02d%02d,1\n", i % 24, i);
    fclose(f);

    reset_cfg_fields();
    clock_config_load();
    CHECK(s_alarm_count == MAX_ALARMS, "alarm list caps at MAX_ALARMS, extra line dropped");
    CHECK(s_alarms[MAX_ALARMS - 1].min == MAX_ALARMS - 1,
          "last accepted alarm is the 8th line in the file, not the 9th");

    remove(test_path(CLOCK_CFG_PATH));
}

/* ---- alarm_delete_at: array-shift bookkeeping used by the in-app editor */
static void test_alarm_delete_at(void)
{
    reset_alarms();
    add_alarm(6, 0, 1); add_alarm(7, 0, 1); add_alarm(8, 0, 1);
    alarm_delete_at(1);   /* remove the middle (07:00) */
    CHECK(s_alarm_count == 2, "count decrements after deleting the middle row");
    CHECK(s_alarms[0].hour == 6 && s_alarms[1].hour == 8,
          "remaining alarms shift down, relative order preserved");

    alarm_delete_at(1);   /* remove the new last row (08:00) */
    CHECK(s_alarm_count == 1 && s_alarms[0].hour == 6,
          "deleting the last row just shrinks the count (no shift needed)");
}

/* ---- next_alarm: tie-break and disabled-skip, not covered by
 * test_clock_alarm.c's soonest/rollover/empty cases */
static void test_next_alarm_extra(void)
{
    reset_alarms();
    add_alarm(9, 0, 1); add_alarm(9, 0, 1);   /* duplicate time, both enabled */
    int idx = -1;
    CHECK(next_alarm(8, 0, &idx) == 60 && idx == 0,
          "duplicate-time alarms: lowest index wins the tie");

    reset_alarms();
    add_alarm(9, 0, 0);   /* disabled: closer, must be skipped entirely */
    add_alarm(10, 0, 1);
    CHECK(next_alarm(8, 0, &idx) == 120 && idx == 1,
          "disabled alarm ignored even though it would be sooner");
}

/* ---- runner state machine: tick_countdown / tick_countup / runner_toggle,
 * shared by Pomodoro/Timer/Stopwatch and untested by test_clock_alarm.c */
static void test_runner_state_machine(void)
{
    runner_t r = { RUN_RUNNING, 5000, 0, 1000 };
    tick_countdown(&r, 3000);   /* dt = 2000 */
    CHECK(r.remaining_ms == 3000 && r.state == RUN_RUNNING, "countdown decrements mid-flight");

    tick_countdown(&r, 6000);   /* dt = 3000 == remaining: lands exactly on zero */
    CHECK(r.remaining_ms == 0 && r.state == RUN_STOPPED,
          "countdown landing exactly on zero stops cleanly");

    r = (runner_t){ RUN_RUNNING, 1000, 0, 0 };
    tick_countdown(&r, 5000);   /* dt = 5000 > remaining: must clamp, not underflow */
    CHECK(r.remaining_ms == 0 && r.state == RUN_STOPPED,
          "countdown overshoot clamps to zero (no unsigned wraparound)");

    r = (runner_t){ RUN_PAUSED, 1000, 0, 0 };
    tick_countdown(&r, 5000);
    CHECK(r.remaining_ms == 1000 && r.state == RUN_PAUSED, "a paused countdown never ticks");

    runner_t w = { RUN_RUNNING, 0, 0, 1000 };
    tick_countup(&w, 4000);
    CHECK(w.elapsed_ms == 3000, "count-up accumulates elapsed time");

    runner_toggle(&w, 4000);
    CHECK(w.state == RUN_PAUSED, "toggle flips running -> paused");
    tick_countup(&w, 9000);
    CHECK(w.elapsed_ms == 3000, "count-up ignored while paused (elapsed frozen)");

    runner_toggle(&w, 9000);
    CHECK(w.state == RUN_RUNNING && w.last_tick == 9000,
          "resume re-anchors last_tick to now (no time-jump on resume)");
    tick_countup(&w, 9500);
    CHECK(w.elapsed_ms == 3500, "count-up resumes correctly after a pause/resume cycle");
}

/* ---- update_pomodoro: work/break cycle transitions + cycle counting */
static void test_update_pomodoro_cycle(void)
{
    s_pomo_work_min = 1; s_pomo_break_min = 1; s_pomo_cycles = 0; s_pomo_on_break = false;
    s_pomo = (runner_t){ RUN_RUNNING, 500, 0, 0 };   /* 0.5s left in the work session */
    s_flash_until = 0;

    update_pomodoro(1000);   /* dt = 1000 > remaining: work session ends this tick */
    CHECK(s_pomo.state == RUN_RUNNING, "the next session (break) auto-starts, never left stopped");
    CHECK(s_pomo_on_break == true,     "work session flips to break");
    CHECK(s_pomo_cycles == 0,          "cycle count does NOT bump on work->break");
    CHECK(s_pomo.remaining_ms == 60000, "break duration loaded (1 min)");
    CHECK(s_flash_until == 1800,       "end-of-session flash armed for 800ms");

    /* finish the break too: this closes one full work+break cycle */
    s_pomo.remaining_ms = 200; s_pomo.last_tick = 1000;
    update_pomodoro(1500);   /* dt = 500 > remaining: break ends */
    CHECK(s_pomo_on_break == false,    "break flips back to work");
    CHECK(s_pomo_cycles == 1,          "cycle count bumps on break->work (one full cycle done)");
    CHECK(s_pomo.remaining_ms == 60000, "work duration reloaded (1 min)");
}

/* ---- update_timer: flash-window re-arm guard */
static void test_update_timer_flash_guard(void)
{
    s_timer = (runner_t){ RUN_RUNNING, 300, 0, 0 };
    s_flash_until = 0;
    update_timer(1000);   /* dt = 1000 > remaining: expires this tick */
    CHECK(s_timer.state == RUN_STOPPED && s_timer.remaining_ms == 0, "timer reaches zero and stops");
    CHECK(s_flash_until == 1800, "flash armed for 800ms after expiry");

    update_timer(1200);   /* still inside the flash window */
    CHECK(s_flash_until == 1800, "flash window is not pushed forward while still active");

    update_timer(1900);   /* past the previous window: guard allows a fresh arm */
    CHECK(s_flash_until == 2700, "flash re-arms once the previous window has fully elapsed");
}

/* ---- clock_edit_time: date/time editor's month/day/year rollover.
 * Drives the REAL interactive loop (input -> render -> repeat) with a
 * scripted gamepad, exactly like the device; only GW_GetUnixTM/SetUnixTM are
 * faked (see rg_rtc.c: those are the clock's only RTC touchpoint). */
static void test_clock_edit_time_rollover(void)
{
    s_hour24 = false;   /* exercise the AM/PM render branch too; doesn't affect saved fields */

    /* 1) Jan 31 -> Feb in a leap year: day must clamp to 29, not stay 31 */
    {
        struct tm init = {0};
        init.tm_year = 124; init.tm_mon = 0; init.tm_mday = 31; init.tm_hour = 10; init.tm_min = 0;
        const int keys[] = { -1, ODROID_INPUT_RIGHT, -1, ODROID_INPUT_UP, -1, ODROID_INPUT_A };
        run_edit_time(init, keys, sizeof keys / sizeof keys[0]);
        CHECK(g_save_called, "A confirms and saves the edited date");
        CHECK(g_saved_tm.tm_mon == 1, "month field advances Jan -> Feb");
        CHECK(g_saved_tm.tm_mday == 29, "day clamps 31 -> 29 for Feb of a leap year (2024)");
        CHECK(g_saved_tm.tm_year == 124, "year untouched by a month-field edit");
    }

    /* 2) Dec -> Jan wrap: month field wraps independently of year (documented
     *    behaviour, not a bug fix target — each field is edited independently,
     *    like a segmented alarm-clock display, so rolling December forward
     *    does NOT bump the year). */
    {
        struct tm init = {0};
        init.tm_year = 124; init.tm_mon = 11; init.tm_mday = 15; init.tm_hour = 10; init.tm_min = 0;
        const int keys[] = { -1, ODROID_INPUT_RIGHT, -1, ODROID_INPUT_UP, -1, ODROID_INPUT_A };
        run_edit_time(init, keys, sizeof keys / sizeof keys[0]);
        CHECK(g_saved_tm.tm_mon == 0,   "month field wraps Dec -> Jan");
        CHECK(g_saved_tm.tm_year == 124, "year stays put on a Dec->Jan month wrap (fields are independent)");
    }

    /* 3) Day field wraps DOWN from day 1 to the last day of the month (Feb of
     *    a leap year -> 29), via the day field's own modular arithmetic. */
    {
        struct tm init = {0};
        init.tm_year = 124; init.tm_mon = 1; init.tm_mday = 1; init.tm_hour = 10; init.tm_min = 0;
        const int keys[] = { -1, ODROID_INPUT_RIGHT, -1, ODROID_INPUT_RIGHT, -1, ODROID_INPUT_DOWN, -1, ODROID_INPUT_A };
        run_edit_time(init, keys, sizeof keys / sizeof keys[0]);
        CHECK(g_saved_tm.tm_mday == 29, "day 1 DOWN wraps to the month's last day (29, leap Feb)");
        CHECK(g_saved_tm.tm_mon == 1 && g_saved_tm.tm_year == 124, "month/year untouched by a day-field edit");
    }

    /* 4) Year field clamps at the editor's ceiling (199 = 2099) instead of
     *    overflowing/wrapping. */
    {
        struct tm init = {0};
        init.tm_year = 199; init.tm_mon = 5; init.tm_mday = 10; init.tm_hour = 0; init.tm_min = 0;
        const int keys[] = { -1, ODROID_INPUT_UP, -1, ODROID_INPUT_A };
        run_edit_time(init, keys, sizeof keys / sizeof keys[0]);
        CHECK(g_saved_tm.tm_year == 199, "year clamps at the max (199 / 2099), does not roll over");
    }

    /* 5) Year field clamps at the editor's floor (70 = 1970). */
    {
        struct tm init = {0};
        init.tm_year = 70; init.tm_mon = 0; init.tm_mday = 1; init.tm_hour = 0; init.tm_min = 0;
        const int keys[] = { -1, ODROID_INPUT_DOWN, -1, ODROID_INPUT_A };
        run_edit_time(init, keys, sizeof keys / sizeof keys[0]);
        CHECK(g_saved_tm.tm_year == 70, "year clamps at the floor (70 / 1970), does not go negative");
    }
}

/* ---- clock_should_idle_sleep: the pure guard rg_clock_show's idle-sleep
 * check calls — see the comment at its definition in rg_clock.c. The clock
 * has no power setting of its own (no keep-awake opt-out): the launcher's
 * global "Idle power off" is the only rule, unconditionally. */
static void test_clock_should_idle_sleep(void)
{
    CHECK(clock_should_idle_sleep(false, MODE_CLOCK, true) == true,
          "sleeps: not ringing, on the clock face, timeout expired");
    CHECK(clock_should_idle_sleep(false, MODE_CLOCK, false) == false,
          "does not sleep: the global timeout has not expired yet");
    CHECK(clock_should_idle_sleep(true, MODE_CLOCK, true) == false,
          "never sleeps while an alarm is ringing");
    CHECK(clock_should_idle_sleep(false, MODE_TIMER, true) == false,
          "never sleeps off the clock face — a running timer is being watched");
    CHECK(clock_should_idle_sleep(false, MODE_POMODORO, true) == false,
          "never sleeps off the clock face — pomodoro");
    CHECK(clock_should_idle_sleep(false, MODE_STOPWATCH, true) == false,
          "never sleeps off the clock face — stopwatch");
}

int main(void)
{
    init_lang();
    test_days_in_month();
    test_cfg_legacy_migration();
    test_cfg_full_roundtrip();
    test_cfg_rejects_out_of_range();
    test_cfg_max_alarms_boundary();
    test_alarm_delete_at();
    test_next_alarm_extra();
    test_runner_state_machine();
    test_update_pomodoro_cycle();
    test_update_timer_flash_guard();
    test_clock_edit_time_rollover();
    test_clock_should_idle_sleep();
    test_ring_overlay();
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
