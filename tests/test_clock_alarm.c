/* Host unit test for the Clock alarm logic: includes rg_clock.c whole so the
 * static functions are directly callable, with stubbed firmware/HAL below. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* redirect /clock.cfg to /tmp/mtest so the test needs no root FS access */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/mtest/t%s", p + 1); return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock.c"
#undef fopen

/* ---- stubs ------------------------------------------------------------- */
static uint32_t fake_tick = 0;
uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_Delay(uint32_t ms) { fake_tick += ms; }
void wdog_refresh(void) {}
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef *h, uint8_t *b, uint16_t l) { (void)h;(void)b;(void)l; return 0; }
int HAL_SAI_DMAStop(SAI_HandleTypeDef *h) { (void)h; return 0; }
SAI_HandleTypeDef hsai_BlockA1; DMA_HandleTypeDef hdma_sai1_a;

static uint16_t fb[GW_LCD_WIDTH * GW_LCD_HEIGHT];
uint16_t *lcd_get_active_buffer(void) { return fb; }
void lcd_swap(void) {}
void lcd_sleep_while_swap_pending(void) {}

static const lang_t L = { .s_AM="AM", .s_PM="PM", .s_Clock="Clock",
  .s_Weekday_Mon="Mon", .s_Weekday_Tue="Tue", .s_Weekday_Wed="Wed", .s_Weekday_Thu="Thu",
  .s_Weekday_Fri="Fri", .s_Weekday_Sat="Sat", .s_Weekday_Sun="Sun",
  .s_Clock_On="On", .s_Clock_Off="Off" };
const lang_t *curr_lang = &L;
int i18n_draw_text_line(int x,int y,int w,const char*t,uint16_t c,uint16_t bg,int f){(void)x;(void)y;(void)w;(void)t;(void)c;(void)bg;(void)f;return 0;}
int odroid_overlay_dialog(const char*h,odroid_dialog_choice_t*o,int s,void(*r)(void),int f){(void)h;(void)o;(void)s;(void)r;(void)f;return -1;}
void odroid_overlay_draw_fill_rect(int x,int y,int w,int h,uint16_t c){(void)x;(void)y;(void)w;(void)h;(void)c;}
void odroid_overlay_draw_text(int x,int y,int w,const char*t,uint16_t c,uint16_t b){(void)x;(void)y;(void)w;(void)t;(void)c;(void)b;}
void odroid_overlay_draw_logo(int x,int y,int l,uint16_t c){(void)x;(void)y;(void)l;(void)c;}
void odroid_overlay_draw_battery(int p,int x,int y){(void)p;(void)x;(void)y;}
int odroid_overlay_get_font_width(void){return 8;}
void odroid_input_read_gamepad(odroid_gamepad_state_t *s){ memset(s,0,sizeof *s); }
int odroid_input_read_battery(void){return 50;}
int GW_GetCurrentHour(void){return 0;} int GW_GetCurrentMinute(void){return 0;}
int GW_GetCurrentSubSeconds(void){return 0;} int GW_GetCurrentMonth(void){return 1;}
int GW_GetCurrentDay(void){return 1;} int GW_GetCurrentWeekday(void){return 1;}
bool clock_gif_ready(void){return false;}
void clock_gif_blit(uint16_t*f,uint32_t n){(void)f;(void)n;}
void clock_gif_load(void){} void clock_gif_free(void){}

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
    s_alarm_vol = 9;
    clock_config_save();
    reset_alarms(); s_alarm_vol = 6;
    clock_config_load();
    CHECK(s_alarm_count == 2, "both alarms survive save/load");
    CHECK(s_alarms[0].hour == 7 && s_alarms[0].min == 30 && s_alarms[0].enabled == 1, "enabled alarm intact");
    CHECK(s_alarms[1].hour == 8 && s_alarms[1].min == 0 && s_alarms[1].enabled == 0, "DISABLED alarm persists (was deleted before fix)");
    CHECK(s_alarm_vol == 9, "volume round-trip");

    FILE *f = fopen(test_path("/clock.cfg"), "w");
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
    s_alarm_vol = 6; s_tone_on = false; dma_state = DMA_TRANSFER_STATE_HF; dma_counter = 100;
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
    test_next_alarm();
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
