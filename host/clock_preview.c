// Host preview harness for the Clock app: renders the REAL rg_clock.c drawing
// code (7-seg ghost digits, top bar, hint bar, alarm-editor popup) into a
// framebuffer and dumps raw RGB565 frames for render_clock.py to PNG-ify.
// Text/logo come from the REAL device assets (sd_content/fonts + rg_logos.c).
//
// Build+run (from repo root):
//   python3 host/gen_logo.py
//   gcc -O2 -std=gnu11 -Ihost -Itests/clock_stubs host/clock_preview.c -o /tmp/clock_preview
//   /tmp/clock_preview && python3 host/render_clock.py
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define W 320
#define H 240

static uint16_t fb[W * H];

/* redirect /clock.cfg away from the host root */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/clockprev_%s", p + 1);
  for (char *c = b + 5; *c; c++) if (*c == '/') *c = '_';   /* flatten subdirs */
  return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock.c"
#undef fopen

/* Text is rendered from the REAL device font bins (sd_content/fonts/*.bin)
 * with the same lookup + LSB-first blit as rg_i18n.c get_font_data /
 * i18n_draw_text_line — so glyphs match the device pixel for pixel.
 * (Default font index 0 = cp1252_serif, same as a fresh device.) */
#include "logo_gnw.h"

/* real GIF pipeline for the preview (host RAM is ample) */
static uint8_t gpool[1024*1024]; static size_t gpool_off = 0;
void *ram_malloc(size_t n){ n=(n+3)&~3u; if(gpool_off+n>sizeof gpool) return 0; void*p=gpool+gpool_off; gpool_off+=n; return p; }
void *ram_calloc(size_t c,size_t n){ void*p=ram_malloc(c*n); if(p) memset(p,0,c*n); return p; }
size_t ram_get_free_size(void){ return sizeof gpool - gpool_off; }
size_t ram_mark(void){ return gpool_off+1; }
void ram_release(size_t m){ gpool_off=m-1; }
#include <fcntl.h>
#include <unistd.h>
#define GIF_PATH "/tmp/mtest/bg.gif"
#include "../Core/Src/porting/lib/gifdec/gifdec.c"
#include "../Core/Src/retro-go/rg_clock_gif.c"
#include "../retro-go-stm32/components/odroid/bitmaps/font_basic.h"

/* ---- framebuffer / lcd ------------------------------------------------- */
uint16_t *lcd_get_active_buffer(void) { return fb; }
void lcd_swap(void) {}
void lcd_sleep_while_swap_pending(void) {}

/* ---- fill rect / text (atlas) ------------------------------------------ */
void odroid_overlay_draw_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            if (px >= 0 && px < W && py >= 0 && py < H) fb[py * W + px] = color;
        }
}

static int utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (*s == 0) return -1;
    int cp, n;
    if (*s < 0x80) { cp = *s; n = 1; }
    else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; n = 2; }
    else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; n = 3; }
    else { cp = *s & 0x07; n = 4; }
    for (int i = 1; i < n; i++) cp = (cp << 6) | (s[i] & 0x3F);
    *p += n;
    return cp;
}

/* mirror of rg_i18n.c get_font_file/get_font_data, reading the same bins */
static int real_glyph(int cp, uint8_t *rows /*12 rows x up to 4B*/, int *cw)
{
    const char *file = "sd_content/fonts/cp1252_serif.bin";
    long off = cp; int fixed = 0, varw_n = 256;
    if (cp >= 0xAC00 && cp <= 0xD7A3) { file = "sd_content/fonts/unicode_hangul.bin"; off = cp - 0xAC00; fixed = 1; }
    else if (cp >= 0x25A0 && cp <= 0x25FF) { file = "sd_content/fonts/unicode_geometric.bin"; off = cp - 0x25A0; varw_n = 96; }
    else if (cp >= 0x4E00 && cp <= 0x9FFF) { file = "sd_content/fonts/unicode_cjk.bin"; off = cp - 0x4E00; fixed = 1; }
    else if (cp >= 0x100) return 0;
    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "missing %s\n", file); return 0; }
    int width; long data_off;
    if (fixed) {
        width = 12;
        data_off = off * ((width + 7) / 8) * 12;
    } else {
        uint8_t w8; fseek(f, off, SEEK_SET); fread(&w8, 1, 1, f); width = w8;
        uint16_t o16; fseek(f, varw_n + off * 2, SEEK_SET); fread(&o16, 1, 2, f);
        data_off = (long)o16 + varw_n * 3;
    }
    int line_bytes = (width + 7) / 8;
    memset(rows, 0, 12 * 4);
    fseek(f, data_off, SEEK_SET);
    for (int y = 0; y < 12; y++) fread(&rows[y * 4], 1, line_bytes, f);
    fclose(f);
    *cw = width;
    return 1;
}

int i18n_get_text_width(const char *t)
{
    const char *p = t; int cp, w = 0; uint8_t rows[12*4];
    while ((cp = utf8_next(&p)) > 0) { int cw; if (real_glyph(cp, rows, &cw)) w += cw; else w += 8; }
    return w;
}

int i18n_draw_text_line(int x, int y, int width, const char *t,
                        uint16_t fg, uint16_t bg, int transparent)
{
    if (!transparent)
        for (int j = 0; j < 12; j++)
            for (int i = 0; i < width; i++)
                if (x + i < W && y + j < H) fb[(y + j) * W + x + i] = bg;
    if (!t) return 12;
    const char *p = t; int cp, x_offset = 0;
    uint8_t rows[12 * 4];
    while ((cp = utf8_next(&p)) > 0) {
        int cw;
        if (!real_glyph(cp, rows, &cw)) continue;
        if (x_offset + cw > width) break;
        for (int j = 0; j < 12; j++) {
            uint32_t bits = rows[j*4] | (rows[j*4+1] << 8) | (rows[j*4+2] << 16) | ((uint32_t)rows[j*4+3] << 24);
            for (int i = 0; i < cw; i++)
                if (bits & (1u << i)) {
                    int px = x + x_offset + i, py = y + j;
                    if (px >= 0 && px < W && py >= 0 && py < H) fb[py * W + px] = fg;
                }
        }
        x_offset += cw;
    }
    return 12;
}

/* ---- misc firmware stubs ------------------------------------------------ */
void wdog_refresh(void) {}
void odroid_system_sleep(void) {}
static uint32_t fake_tick = 0;
uint32_t HAL_GetTick(void) { return fake_tick; }
void HAL_Delay(uint32_t ms) { fake_tick += ms; }
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef *h, uint8_t *b, uint16_t l) { (void)h;(void)b;(void)l; return 0; }
int HAL_SAI_DMAStop(SAI_HandleTypeDef *h) { (void)h; return 0; }
SAI_HandleTypeDef hsai_BlockA1; DMA_HandleTypeDef hdma_sai1_a;
uint32_t audio_mute; int16_t audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2];
dma_transfer_state_t dma_state; uint32_t dma_counter;
uint16_t audio_get_buffer_full_length(void){ return AUDIO_BUFFER_LENGTH*2; }
uint16_t audio_get_buffer_length(void){ return AUDIO_BUFFER_LENGTH; }
uint16_t audio_get_buffer_size(void){ return AUDIO_BUFFER_LENGTH*2; }
int16_t *audio_get_active_buffer(void){ return audiobuffer_dma; }
int16_t *audio_get_inactive_buffer(void){ return audiobuffer_dma; }
void audio_clear_active_buffer(void){} void audio_clear_inactive_buffer(void){}
void audio_clear_buffers(void){} void audio_set_buffer_length(uint16_t l){(void)l;}
void audio_start_playing(uint16_t l){(void)l;} void audio_start_playing_full_length(uint16_t l){(void)l;}
void audio_stop_playing(void){}

/* system-volume stubs (alarm loudness follows the global volume) */
const uint8_t volume_tbl[ODROID_AUDIO_VOLUME_MAX + 1] =
    { 0, 4, 8, 15, 32, 48, 64, 96, 128, 255 };
static int stub_volume = 9;
int odroid_audio_volume_get(void) { return stub_volume; }
void odroid_audio_volume_set(int level) { stub_volume = level; }

/* the real 35x30 G&W logo bitmap served through the firmware API */
retro_logo_image *rg_get_logo(int16_t idx)
{
    (void)idx;
    static uint8_t buf[sizeof(retro_logo_image) + sizeof(LOGO_BITS)];
    retro_logo_image *lg = (retro_logo_image *)buf;
    if (lg->width == 0) {
        lg->width = LOGO_W; lg->height = LOGO_H;
        memcpy(lg->logo, LOGO_BITS, sizeof(LOGO_BITS));
    }
    return lg;
}

void odroid_overlay_draw_logo(int x, int y, int logo, uint16_t color)
{
    (void)logo;
    retro_logo_image *lg = rg_get_logo(0);
    int wbytes = (lg->width + 7) / 8;
    for (int j = 0; j < lg->height; j++)
        for (int i = 0; i < wbytes; i++) {
            unsigned char g = (unsigned char)lg->logo[j * wbytes + i];
            for (int b = 0; b < 8; b++)
                if ((g >> (7 - b)) & 1) {
                    int px = x + i * 8 + b, py = y + j;
                    if (px >= 0 && px < W && py >= 0 && py < H) fb[py * W + px] = color;
                }
        }
}

void odroid_overlay_draw_battery(int pct, int x, int y)
{
    uint16_t c = 0xFD20;
    for (int i = 0; i < 20; i++) { fb[y*W + x+i] = c; fb[(y+9)*W + x+i] = c; }
    for (int j = 0; j < 10; j++) { fb[(y+j)*W + x] = c; fb[(y+j)*W + x+19] = c; }
    int wfill = pct * 16 / 100;
    for (int j = 2; j < 8; j++) for (int i = 0; i < wfill; i++) fb[(y+j)*W + x+2+i] = c;
    for (int j = 3; j < 7; j++) fb[(y+j)*W + x+20] = c;
}

/* the REAL firmware 8x8 font path (font8x8_basic, bg painted per cell) */
void odroid_overlay_draw_text(int x, int y, int w, const char *text, uint16_t color, uint16_t bg)
{
    int text_len = (int)strlen(text);
    for (int i = 0; i < w / 8; i++) {
        const char *glyph = font8x8_basic[(i < text_len) ? (unsigned char)text[i] : ' '];
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++) {
                int px = x + i*8 + xx, py = y + yy;
                if (px >= 0 && px < W && py >= 0 && py < H)
                    fb[py * W + px] = (glyph[yy] & (1 << xx)) ? color : bg;
            }
    }
}
int odroid_overlay_get_font_width(void) { return 8; }
int odroid_overlay_dialog(const char *h, odroid_dialog_choice_t *o, int s, void (*r)(void), int f)
{ (void)h;(void)o;(void)s;(void)r;(void)f; return -1; }

static odroid_gamepad_state_t stub_pad;
void odroid_input_read_gamepad(odroid_gamepad_state_t *s){ *s = stub_pad; }
int odroid_input_read_battery(void){ return 76; }

static int st_h = 9, st_m = 41, st_mon = 7, st_day = 6, st_wd = 1, st_ss = 100;
int GW_GetCurrentHour(void){return st_h;} int GW_GetCurrentMinute(void){return st_m;}
int GW_GetCurrentSubSeconds(void){return st_ss;} int GW_GetCurrentMonth(void){return st_mon;}
int GW_GetCurrentDay(void){return st_day;} int GW_GetCurrentWeekday(void){return st_wd;}


/* Korean UI strings (what the device shows with lang=ko) */
static const lang_t KO = {
    .s_AM="오전", .s_PM="오후", .s_Clock="시계",
    .s_Weekday_Mon="월", .s_Weekday_Tue="화", .s_Weekday_Wed="수", .s_Weekday_Thu="목",
    .s_Weekday_Fri="금", .s_Weekday_Sat="토", .s_Weekday_Sun="일",
    .s_Clock_Pomodoro="뽀모도로", .s_Clock_Timer="타이머", .s_Clock_Stopwatch="스톱워치",
    .s_Clock_Work="집중", .s_Clock_Break="휴식", .s_Clock_Cycle="라운드",
    .s_Clock_Ringing="알람!",
    .s_Clock_Hint_Clock="PAUSE 설정",
    .s_Clock_Hint_Run="A 시작/정지  B 리셋",
    .s_Clock_Hint_Stop="A 시작/정지  B 리셋",
    .s_Clock_Hint_TimerStop="A 시작/정지  B 리셋  위아래 분",
    .s_Clock_Hint_Editor="A 편집  TIME 켬/끔  GAME 삭제  B 완료",
    .s_Clock_Hint_Edit="좌우 자리  위아래 조절  A 확인  B 취소",
    .s_Clock_Add_Alarm="알람 추가", .s_Clock_Done="완료",
    .s_Clock_On="켬", .s_Clock_Off="끔",
    .s_Clock_Format="시간 형식", .s_Clock_DND="방해 금지",
    .s_Clock_Anim="배경 효과", .s_Clock_Anim_0="끄기 (배터리 소모 없음)",
    .s_Clock_Anim_1="은은한 효과 (낮음)", .s_Clock_Anim_2="픽셀 풍경 (낮음)", .s_Clock_Anim_3="GIF (높음)",
    .s_Clock_Volume="알람 음량", .s_Clock_Alarms="알람", .s_Clock_Exit="시계 나가기",
    .s_Clock_Hint_Ring="A 5분 스누즈  B 끄기", .s_Clock_Theme="테마", .s_Clock_Face="숫자 서체", .s_Clock_Auto="자동", .s_Full="\x7", .s_Fill="\x8",
};
const lang_t *curr_lang = &KO;

/* ---- scene dump ---------------------------------------------------------- */
static void dump(const char *name)
{
    char path[128]; snprintf(path, sizeof path, "/tmp/clockprev/%s.bin", name);
    FILE *f = fopen(path, "wb");   /* note: macro-redirected is fine too */
    if (!f) { perror(path); return; }
    fwrite(fb, 2, W * H, f);
    fclose(f);
    printf("wrote %s\n", path);
}

static void base(uint16_t bg) { for (int i = 0; i < W * H; i++) fb[i] = bg; }

static void paint(clock_mode_t mode, uint32_t now, bool ringing)
{
    base(TH()->scr);
    if (s_anim == ANIM_GIF && clock_gif_ready()) clock_gif_blit(fb, now);
    else if (s_anim == ANIM_SCENE) draw_scene(now, TH());
    else if (s_anim == 1) draw_ambient(now, TH()->ink);
    switch (mode) {
    case MODE_CLOCK:     render_clock(now, ringing); break;
    case MODE_POMODORO:  render_pomodoro(now);  break;
    case MODE_TIMER:     render_timer(now);     break;
    case MODE_STOPWATCH: render_stopwatch(now); break;
    default: break;
    }
    draw_topbar(mode);
    draw_hintbar(ringing ? curr_lang->s_Clock_Hint_Ring
        : mode == MODE_CLOCK     ? curr_lang->s_Clock_Hint_Clock
        : mode == MODE_POMODORO  ? (s_pomo.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                : curr_lang->s_Clock_Hint_TimerStop)
        : mode == MODE_TIMER     ? (s_timer.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                 : curr_lang->s_Clock_Hint_TimerStop)
        : curr_lang->s_Clock_Hint_Run);
    if (ringing && mode != MODE_CLOCK && ((now / 200) & 1)) {
        int bw = i18n_get_text_width(curr_lang->s_Clock_Ringing);
        int bx = (W - bw) / 2;
        draw_icon(&PIX_BELL, bx - 18, 43, TH()->alarm);
        draw_icon(&PIX_BELL, bx + bw + 7, 43, TH()->alarm);
        draw_centered_i18n(42, curr_lang->s_Clock_Ringing, TH()->alarm);
    }
}

int main(void)
{
    /* one enabled alarm so the next-alarm chip shows */
    s_alarms[0] = (alarm_t){ 7, 30, 1 };
    s_alarms[1] = (alarm_t){ 22, 15, 0 };
    s_alarm_count = 2;
    s_hour24 = false;

    /* 1) clock face */
    paint(MODE_CLOCK, 1000, false); dump("clock_hints");
    paint(MODE_CLOCK, 1000, false); dump("clock_clean");
    /* 2) clock with ambient dots + DND moon */
    s_anim = 1; s_dnd = true; paint(MODE_CLOCK, 1000, false); dump("clock_ambient"); s_anim = 0; s_dnd = false;
    /* 2b) built-in pixel scene */
    s_anim = ANIM_SCENE; paint(MODE_CLOCK, 1000, false); dump("clock_scene"); s_anim = 0;
    /* 3) clock ringing (accent pulse frame + snooze legend) */
    paint(MODE_CLOCK, 1200, true); dump("clock_ringing");
    /* 4) pomodoro running (13:37 left of work round 2) */
    s_pomo.state = RUN_RUNNING; s_pomo.remaining_ms = (13*60+37)*1000; s_pomo_cycles = 1;
    paint(MODE_POMODORO, 1000, false); dump("pomodoro");
    /* 5) timer stopped at 05:00 */
    paint(MODE_TIMER, 1000, false); dump("timer");
    /* 6) stopwatch running at 12:34 */
    s_watch.state = RUN_RUNNING; s_watch.elapsed_ms = (12*60+34)*1000;
    paint(MODE_STOPWATCH, 1200, false); dump("stopwatch");
    /* 6b) Amber theme (pixel face) — theme/face pickers are back */
    s_theme = 1; paint(MODE_CLOCK, 1000, false); dump("clock_amber"); s_theme = 0;
    /* 6c) GIF background (real decode; device gates on free RAM) */
    s_anim = ANIM_GIF; clock_gif_load();
    paint(MODE_CLOCK, 0, false); dump("clock_gif");
    clock_gif_free(); s_anim = 0;
    /* 7) alarm editor popup (row 0 selected) */
    render_alarm_setup(0); dump("editor");
    /* 8) full-screen clone-view alarm editor (hour field lit phase) */
    render_alarm_edit(&s_alarms[0], 0, false); dump("editor_edit");
    return 0;
}
