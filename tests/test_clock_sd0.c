/* Flash-build (SD_CARD=0) coverage for the Clock app: compiles the SAME
 * rg_clock.c as the other clock tests but with -DSD_CARD=0, so the user-media
 * features (photo album / GIF background / MP3-WAV alarm) are compiled OUT.
 *
 * Two things are proven here that the SD-card tests cannot:
 *   1. clock_config_load() CLAMPS a saved GIF/photo background (anim=3/4) back
 *      to Off on a flash build — a card-less unit can never show those, so a cfg
 *      copied from an SD unit must not leave the picker on an invalid value.
 *   2. clock_config_save() does NOT emit the SD-only keys (photospeed/bgfile/
 *      alarmsnd), and a valid on-both value (Scene) round-trips untouched.
 *
 * The stub surface mirrors test_clock_more.c (same firmware seam); the media
 * stubs are inert here because rg_clock.c never references them on SD_CARD=0.
 *
 * Build + run (wired into tests/run.sh):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=0 -Itests/clock_stubs \
 *       tests/test_clock_sd0.c -o /tmp/mtest/test_clock_sd0 && /tmp/mtest/test_clock_sd0
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* redirect /clock.cfg to /tmp/mtest so the test needs no root FS access
 * (identical trick to test_clock_more.c) */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/mtest/s%s", p + 1);
  for (char *c = b + 11; *c; c++) if (*c == '/') *c = '_';   /* flatten subdirs */
  return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock.c"
#undef fopen

/* ---- firmware-seam stubs (superset of what SD_CARD=0 rg_clock.c needs) --- */
static uint32_t fake_tick = 0;
uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_Delay(uint32_t ms) { fake_tick += ms; }
void wdog_refresh(void) {}
void odroid_system_sleep(void) {}
/* media engines — never called on SD_CARD=0 (compiled out), inert if they were */
bool clock_album_open(void) { return false; }
bool clock_album_ready(void) { return false; }
const uint16_t *clock_album_current(void) { return 0; }
void clock_album_advance(void) {}
int clock_album_count(void) { return 0; }
void clock_album_close(void) {}
bool clock_gif_ready(void){return false;}
void clock_gif_blit(uint16_t*f,uint32_t n){(void)f;(void)n;}
bool clock_gif_load(void){return false;} void clock_gif_free(void){}
int clock_gif_status(void){return 1;}
const char *clock_gif_diag(void){return "";}
void clock_gif_set_file(const char *n){ (void)n; }
bool clock_alarm_mp3_available(void){ return false; }
bool clock_alarm_mp3_start(void){ return false; }
void clock_alarm_mp3_service(int v){ (void)v; }
void clock_alarm_mp3_stop(void){}
bool clock_alarm_mp3_active(void){ return false; }
void clock_alarm_mp3_set_file(const char *n){ (void)n; }
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
 * timeout — never expired, so it is not exercised by the SD0 clamp tests. */
bool odroid_idle_timeout_expired(uint32_t idle_seconds) { (void)idle_seconds; return false; }
/* rg_main.c's PAUSE-menu row, reused (not copied) by the clock's own settings
 * menu -- rg_main.c isn't compiled here, so link a stub. */
bool main_menu_timeout_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)e; (void)r; if (o && o->value) o->value[0] = 0; return false; }

static lang_t L;
static void init_lang(void)
{ const char **p = (const char **)&L;
  for (size_t i = 0; i < sizeof(L) / sizeof(const char *); i++) p[i] = "x"; }
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
int odroid_input_read_battery(void){return 50;}
int GW_GetCurrentHour(void){return 0;} int GW_GetCurrentMinute(void){return 0;}
int GW_GetCurrentSubSeconds(void){return 0;} int GW_GetCurrentMonth(void){return 1;}
int GW_GetCurrentDay(void){return 1;} int GW_GetCurrentWeekday(void){return 1;}

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
void odroid_input_read_gamepad(odroid_gamepad_state_t *s) { memset(s, 0, sizeof *s); }
void GW_GetUnixTM(struct tm *tm) { memset(tm, 0, sizeof *tm); }
void GW_SetUnixTM(struct tm *tm) { (void)tm; }

/* ---- tests --------------------------------------------------------------*/
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

/* Write a raw cfg body then load it through rg_clock's parser. */
static void write_cfg(const char *body)
{ FILE *f = fopen(test_path(CLOCK_CFG_PATH), "w"); if (f) { fputs(body, f); fclose(f); } }

static bool file_contains(const char *needle)
{
    FILE *f = fopen(test_path(CLOCK_CFG_PATH), "r"); if (!f) return false;
    char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    init_lang();
    remove(test_path(CLOCK_CFG_PATH));
    remove(test_path(CLOCK_CFG_LEGACY));

    /* 1) a GIF background saved on an SD unit clamps to Off on a flash build */
    write_cfg("anim=3\nscene=4\n");
    clock_config_load();
    CHECK(s_anim == 0,  "saved GIF anim (3) clamps to Off on flash build");
    CHECK(s_scene == 4, "scene value survives the clamp");

    /* 2) a photo-album background likewise clamps to Off */
    write_cfg("anim=4\n");
    clock_config_load();
    CHECK(s_anim == 0, "saved Photo anim (4) clamps to Off on flash build");

    /* 3) the on-both Scene value round-trips untouched */
    write_cfg("anim=2\nscene=3\n");
    clock_config_load();
    CHECK(s_anim == ANIM_SCENE, "Scene anim (2) is kept on flash build");
    CHECK(s_scene == 3,         "scene index round-trips");

    /* 4) retired ambient still migrates to Off */
    write_cfg("anim=1\n");
    clock_config_load();
    CHECK(s_anim == 0, "retired ambient (1) migrates to Off");

    /* 5) save must NOT emit SD-only MEDIA keys on a flash build, but DOES now
     * persist the alarm sound as a synth-preset token (selectable on SD0 too) */
    s_anim = ANIM_SCENE; s_scene = 5;
    clock_config_save();
    CHECK(!file_contains("photospeed="), "flash save omits photospeed=");
    CHECK(!file_contains("bgfile="),     "flash save omits bgfile=");
    CHECK(file_contains("alarmsnd="),    "flash save writes the synth-preset token");
    CHECK(file_contains("anim=2"),       "flash save still writes anim/scene/theme");

    /* 6) the synth preset round-trips through the alarmsnd= token on flash */
    write_cfg("alarmsnd=Chirp\n");
    clock_config_load();
    CHECK(s_beep_preset == RG_TONE_CHIRP, "flash alarmsnd= token selects the synth preset");

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
