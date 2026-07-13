/* Host unit test for the Clock alarm logic: includes rg_clock.c whole so the
 * static functions are directly callable, with stubbed firmware/HAL below. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* redirect /clock.cfg to /tmp/mtest so the test needs no root FS access */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/mtest/t%s", p + 1);
  for (char *c = b + 11; *c; c++) if (*c == '/') *c = '_';   /* flatten subdirs */
  return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock.c"
#undef fopen

/* ---- stubs ------------------------------------------------------------- */
static uint32_t fake_tick = 0;
uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_Delay(uint32_t ms) { fake_tick += ms; }
void wdog_refresh(void) {}
void odroid_system_sleep(void) {}
/* photo album + launcher list rebuild — inert on the host */
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
/* rg_clock_show() (unused here, but compiled) asks the launcher's global idle
 * timeout — never expired, so it is not exercised by the alarm tests. */
bool odroid_idle_timeout_expired(uint32_t idle_seconds) { (void)idle_seconds; return false; }
/* rg_main.c's PAUSE-menu row, reused (not copied) by the clock's own settings
 * menu (see rg_clock.c's clock_settings_menu) -- rg_main.c isn't compiled
 * here, so link a stub; nothing in this file's tests exercises the row. */
bool main_menu_timeout_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)e; (void)r; if (o && o->value) o->value[0] = 0; return false; }

static const lang_t L = { .s_AM="AM", .s_PM="PM", .s_Clock="Clock",
  .s_Weekday_Mon="Mon", .s_Weekday_Tue="Tue", .s_Weekday_Wed="Wed", .s_Weekday_Thu="Thu",
  .s_Weekday_Fri="Fri", .s_Weekday_Sat="Sat", .s_Weekday_Sun="Sun",
  .s_Clock_On="On", .s_Clock_Off="Off" };
const lang_t *curr_lang = &L;
int i18n_draw_text_line(int x,int y,int w,const char*t,uint16_t c,uint16_t bg,int f){(void)x;(void)y;(void)w;(void)t;(void)c;(void)bg;(void)f;return 0;}
int i18n_get_text_width(const char *t){ return (int)strlen(t) * 6; }
int odroid_overlay_dialog(const char*h,odroid_dialog_choice_t*o,int s,void(*r)(void),int f){(void)h;(void)o;(void)s;(void)r;(void)f;return -1;}
void odroid_overlay_draw_fill_rect(int x,int y,int w,int h,uint16_t c){(void)x;(void)y;(void)w;(void)h;(void)c;}
void odroid_overlay_draw_text(int x,int y,int w,const char*t,uint16_t c,uint16_t b){(void)x;(void)y;(void)w;(void)t;(void)c;(void)b;}
void odroid_overlay_draw_logo(int x,int y,int l,uint16_t c){(void)x;(void)y;(void)l;(void)c;}
retro_logo_image *rg_get_logo(int16_t i){ (void)i; return NULL; }
void odroid_overlay_draw_battery(int p,int x,int y){(void)p;(void)x;(void)y;}
int odroid_overlay_get_font_width(void){return 8;}
void odroid_input_read_gamepad(odroid_gamepad_state_t *s){ memset(s,0,sizeof *s); }
int odroid_input_read_battery(void){return 50;}
void GW_GetUnixTM(struct tm *tm){ memset(tm,0,sizeof *tm); tm->tm_hour=9; tm->tm_min=41; }
void GW_SetUnixTM(struct tm *tm){ (void)tm; }
int GW_GetCurrentHour(void){return 0;} int GW_GetCurrentMinute(void){return 0;}
int GW_GetCurrentSubSeconds(void){return 0;} int GW_GetCurrentMonth(void){return 1;}
int GW_GetCurrentDay(void){return 1;} int GW_GetCurrentWeekday(void){return 1;}
bool clock_gif_ready(void){return false;}
void clock_gif_blit(uint16_t*f,uint32_t n){(void)f;(void)n;}
bool clock_gif_load(void){return false;} void clock_gif_free(void){}
int clock_gif_status(void){return 1;}
const char *clock_gif_diag(void){return "";}
void clock_gif_set_file(const char *n){ (void)n; }

/* MP3-alarm engine — controllable stubs (its own logic is tested in
 * tests/test_clock_mp3.c). Here we only need to steer ring_audio's source pick. */
static int stub_mp3_avail = 0, stub_mp3_start_ok = 0;
static int mp3_service_calls = 0, mp3_stop_calls = 0, mp3_active = 0, mp3_service_vol = -1;
bool clock_alarm_mp3_available(void){ return stub_mp3_avail; }
bool clock_alarm_mp3_start(void){ mp3_active = stub_mp3_start_ok; return stub_mp3_start_ok; }
void clock_alarm_mp3_service(int v){ mp3_service_calls++; mp3_service_vol = v; }
void clock_alarm_mp3_stop(void){ mp3_stop_calls++; mp3_active = 0; }
bool clock_alarm_mp3_active(void){ return mp3_active; }
void clock_alarm_mp3_set_file(const char *n){ (void)n; }

/* simulated SAI DMA (mirrors gw_audio.c semantics) */
uint32_t audio_mute;
int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2];
dma_transfer_state_t dma_state; uint32_t dma_counter;
static uint16_t full_len = AUDIO_BUFFER_LENGTH * 2;
static int sim_playing = 0, sim_starts = 0, sim_stops = 0;
uint16_t audio_get_buffer_full_length(void){ return full_len; }
uint16_t audio_get_buffer_length(void){ return full_len / 2; }
uint16_t audio_get_buffer_size(void){ return audio_get_buffer_length()*2; }
int16_t *audio_get_active_buffer(void){ return &audiobuffer_dma[dma_state == DMA_TRANSFER_STATE_HF ? 0 : full_len/2]; }
int16_t *audio_get_inactive_buffer(void){ return &audiobuffer_dma[dma_state == DMA_TRANSFER_STATE_TC ? 0 : full_len/2]; }
void audio_clear_active_buffer(void){ memset(audio_get_active_buffer(),0,audio_get_buffer_size()); }
void audio_clear_inactive_buffer(void){ memset(audio_get_inactive_buffer(),0,audio_get_buffer_size()); }
void audio_clear_buffers(void){ memset(audiobuffer_dma,0,sizeof audiobuffer_dma); }
void audio_set_buffer_length(uint16_t l){ full_len = l*2; }
void audio_start_playing(uint16_t l){ audio_clear_buffers(); full_len = l*2; sim_playing = 1; sim_starts++; }
void audio_start_playing_full_length(uint16_t l){ audio_clear_buffers(); full_len = l; sim_playing = 1; sim_starts++; }
void audio_stop_playing(void){ audio_clear_buffers(); sim_playing = 0; sim_stops++; }

/* system-volume stubs (alarm loudness follows the global volume) */
const uint8_t volume_tbl[ODROID_AUDIO_VOLUME_MAX + 1] =
    { 0, 4, 8, 15, 32, 48, 64, 96, 128, 255 };
static int stub_volume = 9;
int odroid_audio_volume_get(void) { return stub_volume; }
void odroid_audio_volume_set(int level) { stub_volume = level; }
static void sim_isr_flip(void)  /* one DMA half consumed */
{ dma_state = (dma_state == DMA_TRANSFER_STATE_HF) ? DMA_TRANSFER_STATE_TC : DMA_TRANSFER_STATE_HF; dma_counter++; }

/* ---- tests ------------------------------------------------------------- */
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

static void reset_alarms(void)
{ s_alarm_count = 0; s_dnd = false; s_last_fired_min = -1; memset(s_alarms,0,sizeof s_alarms); }
static void add_alarm(int h, int m, int en)
{ s_alarms[s_alarm_count++] = (alarm_t){ (uint8_t)h, (uint8_t)m, (uint8_t)en }; }

static void test_refire_next_day(void)
{
    reset_alarms(); add_alarm(7, 0, 1);
    CHECK(alarm_should_fire(7, 0) == true,  "fires at 07:00");
    CHECK(alarm_should_fire(7, 0) == false, "no re-fire same minute");
    CHECK(alarm_should_fire(7, 1) == false, "quiet at 07:01");
    /* clock wraps to the same minute a day later (bedside clock left running) */
    CHECK(alarm_should_fire(6, 59) == false, "quiet at 06:59 next day");
    CHECK(alarm_should_fire(7, 0) == true,  "re-fires next day at 07:00");
}

static void test_dnd(void)
{
    reset_alarms(); add_alarm(7, 0, 1); s_dnd = true;
    CHECK(alarm_should_fire(7, 0) == false, "DND suppresses");
    s_dnd = false;
    CHECK(alarm_should_fire(7, 0) == true,  "fires same minute once DND lifted");
}

static void test_window(void)
{
    reset_alarms(); add_alarm(10, 3, 1);
    CHECK(alarm_fired_in_window(600, 605) == true,  "10:03 in (10:00,10:05)");
    CHECK(alarm_fired_in_window(603, 605) == false, "10:03 == from excluded");
    reset_alarms(); add_alarm(10, 5, 1);
    CHECK(alarm_fired_in_window(600, 605) == false, "to_mod exclusive (regular check owns it)");
    reset_alarms(); add_alarm(0, 0, 1);
    CHECK(alarm_fired_in_window(23*60+58, 2) == true, "midnight wrap");
    reset_alarms(); add_alarm(10, 3, 0);
    CHECK(alarm_fired_in_window(600, 605) == false, "disabled alarm ignored");
    reset_alarms(); add_alarm(10, 3, 1); s_dnd = true;
    CHECK(alarm_fired_in_window(600, 605) == false, "DND blocks window catch-up");
    s_dnd = false;
    CHECK(alarm_fired_in_window(605, 605) == false, "zero-length window");
}

static void test_cfg_roundtrip(void)
{
    reset_alarms(); add_alarm(7, 30, 1); add_alarm(8, 0, 0);
    s_dnd = true;
    clock_config_save();
    reset_alarms(); s_dnd = false;
    clock_config_load();
    CHECK(s_alarm_count == 2, "both alarms survive save/load");
    CHECK(s_alarms[0].hour == 7 && s_alarms[0].min == 30 && s_alarms[0].enabled == 1, "enabled alarm intact");
    CHECK(s_alarms[1].hour == 8 && s_alarms[1].min == 0 && s_alarms[1].enabled == 0, "DISABLED alarm persists (was deleted before fix)");
    CHECK(s_dnd == true, "dnd round-trip");

    /* Write to the primary cfg path the loader reads first — the earlier
     * save() above already left a file there, so writing to the legacy
     * /clock.cfg would be shadowed and the parser never exercised. */
    FILE *f = fopen(test_path(CLOCK_CFG_PATH), "w");
    fprintf(f, "alarm=0730\n");        /* old format = enabled */
    fprintf(f, "alarm=0775\n");        /* minute 75: reject */
    fprintf(f, "alarm=2500\n");        /* hour 25: reject */
    fprintf(f, "alarm=-5\n");          /* negative: reject */
    fprintf(f, "alarm=0810,0\n");      /* new format, disabled */
    fclose(f);
    reset_alarms();
    clock_config_load();
    CHECK(s_alarm_count == 2, "junk lines rejected, valid kept");
    CHECK(s_alarms[0].hour == 7 && s_alarms[0].min == 30 && s_alarms[0].enabled == 1, "old format loads enabled");
    CHECK(s_alarms[1].hour == 8 && s_alarms[1].min == 10 && s_alarms[1].enabled == 0, "new format keeps disabled");
}

static void test_tone_dma_sync(void)
{
    s_tone_on = false; dma_state = DMA_TRANSFER_STATE_HF; dma_counter = 100;
    tone_feed(0, true);
    CHECK(sim_starts == 1 && s_tone_on, "tone starts on first ring feed");
    uint32_t phase_after_first = s_tone_phase;
    CHECK(phase_after_first > 0, "first half filled immediately");
    tone_feed(8, true); tone_feed(16, true);   /* same half still playing */
    CHECK(s_tone_phase == phase_after_first, "no rewrite while dma_counter unchanged");
    sim_isr_flip();                            /* ISR frees the other half */
    tone_feed(24, true);
    int per = AUDIO_SAMPLE_RATE / TONE_HZ;
    uint32_t expect = (phase_after_first + AUDIO_BUFFER_LENGTH) % (uint32_t)per;
    CHECK(s_tone_phase == expect, "next half filled once, phase continuous");
    /* square wave continuity across the half boundary: no truncated run */
    int period = AUDIO_SAMPLE_RATE / TONE_HZ, half_period = period / 2;
    int16_t *b = audiobuffer_dma; int n = AUDIO_BUFFER_LENGTH * 2, bad = 0, run = 1;
    for (int i = 1; i < n; i++) {
        if (b[i] == b[i-1]) run++;
        else { if (run != half_period && run != period - half_period) bad++; run = 1; }
    }
    CHECK(bad <= 1, "phase-continuous square across DMA halves");   /* <=1: final partial run */
    tone_feed(32, false);
    CHECK(sim_stops == 1 && !s_tone_on, "tone stops on dismiss");
    int16_t sum = 0; for (int i = 0; i < n; i++) sum |= b[i];
    CHECK(sum == 0, "buffer silenced after stop");
}

/* ---- alarm volume: the clock's OWN loudness (s_alarm_volume), independent
 * of the system volume (stub_volume / odroid_audio_volume_get above) — a
 * quiet gaming system volume must not silently mute the morning alarm, and
 * 0 must mean an intentionally SILENT alarm (amp 0), not "same as max". */
static void test_alarm_volume_amp(void)
{
    stub_volume = 9;   /* system volume at max: must have NO effect on the beep */

    s_alarm_volume = 0;
    s_tone_on = false; dma_state = DMA_TRANSFER_STATE_HF; dma_counter = 200;
    tone_feed(0, true);
    int16_t *b0 = audiobuffer_dma; int n = AUDIO_BUFFER_LENGTH * 2;
    int16_t sum0 = 0; for (int i = 0; i < n; i++) sum0 |= b0[i];
    CHECK(sum0 == 0, "alarm volume 0 means a silent beep (amp 0), even at max system volume");
    tone_feed(8, false);

    s_alarm_volume = 9;   /* max alarm volume: must actually produce sound */
    s_tone_on = false; dma_state = DMA_TRANSFER_STATE_HF; dma_counter = 300;
    tone_feed(0, true);
    int16_t *b9 = audiobuffer_dma;
    bool any_nonzero = false; for (int i = 0; i < n; i++) if (b9[i] != 0) any_nonzero = true;
    CHECK(any_nonzero, "alarm volume 9 produces a non-silent beep");
    tone_feed(8, false);

    s_alarm_volume = 6;   /* restore the module default for later tests */
}

/* ring_audio picks the source ONCE at ring start: MP3 file -> MP3, else beep;
 * a failed MP3 start falls straight back to the beep (never a silent alarm). */
static void test_ring_source(void)
{
    /* no alarm file -> synth beep */
    s_ring_mp3 = false; s_tone_on = false; s_anim = 0; s_album_used = false;
    stub_mp3_avail = 0; mp3_service_calls = 0;
    dma_state = DMA_TRANSFER_STATE_HF; dma_counter = 1;
    ring_audio(0, true);
    CHECK(!s_ring_mp3 && s_tone_on && mp3_service_calls == 0, "ring: beep when no MP3 file");

    /* alarm file present + decodes -> MP3, beep untouched, lists flagged dirty */
    s_ring_mp3 = false; s_tone_on = false; s_album_used = false;
    stub_mp3_avail = 1; stub_mp3_start_ok = 1;
    mp3_service_calls = 0; mp3_stop_calls = 0;
    ring_audio(0, true);
    CHECK(s_ring_mp3 && !s_tone_on && mp3_service_calls == 1, "ring: MP3 when file present + decodable");
    CHECK(s_album_used, "ring: MP3 flags shared_files dirty (lists rebuilt on exit)");
    ring_audio(0, false);   /* dismiss */
    CHECK(!s_ring_mp3 && mp3_stop_calls == 1, "ring: dismiss stops the MP3");

    /* file present but decode fails -> fall back to the beep */
    s_ring_mp3 = false; s_tone_on = false;
    stub_mp3_avail = 1; stub_mp3_start_ok = 0; mp3_service_calls = 0;
    ring_audio(0, true);
    CHECK(!s_ring_mp3 && s_tone_on && mp3_service_calls == 0, "ring: undecodable MP3 falls back to beep");
}

static void test_next_alarm(void)
{
    reset_alarms(); add_alarm(7, 0, 1); add_alarm(22, 30, 1);
    int idx = -1;
    CHECK(next_alarm(21, 0, &idx) == 90 && idx == 1, "soonest alarm picked");
    CHECK(next_alarm(23, 0, &idx) == 8*60 && idx == 0, "day rollover to 07:00");
    CHECK(next_alarm(7, 0, &idx) == 22*60+30 - 7*60 && idx == 1,
          "alarm==now rolls to tomorrow, so 22:30 is soonest");
    reset_alarms();
    CHECK(next_alarm(12, 0, &idx) == -1, "none when empty");
}

int main(void)
{
    test_refire_next_day();
    test_dnd();
    test_window();
    test_cfg_roundtrip();
    test_tone_dma_sync();
    test_alarm_volume_amp();
    test_ring_source();
    test_next_alarm();
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
