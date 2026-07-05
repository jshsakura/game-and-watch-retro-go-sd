/* Full-screen Clock app — see rg_clock.h.
 *
 * A mode switcher (Left/Right) over: an alarm-clock face benchmarked on the
 * Game & Watch clock — retro-go's own G&W logo top-left, date + weekday, big
 * time with AM/PM, the next alarm below — plus Pomodoro, a countdown timer and
 * a stopwatch. Everything is drawn procedurally (7-segment / pixel digit
 * faces), so there is no bundled art. Themes are colour+face sets; the date
 * and AM/PM come straight from the firmware i18n table so they follow the
 * selected language. Config (theme, 24h, DND, alarms) lives in /clock.cfg.
 *
 * Runs inside the launcher context (no APPID overlay), so it costs a handful
 * of bytes of RAM and can never reduce an emulator's heap or DTCM. The paint
 * loop only redraws when the visible frame changes, to spare the battery. */

#include <odroid_system.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "rg_rtc.h"
#include "rg_i18n.h"
#include "gui.h"
#include "bitmaps.h"
#include "odroid_overlay.h"
#include "odroid_input.h"
#include "rg_clock.h"

#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define CLOCK_BLACK 0x0000

/* ---- themes: a colour + digit-face set (chosen with Up/Down, saved) ------ */

typedef enum { FACE_SEG7 = 0, FACE_PIXEL, FACE_DOT } digit_face_t;

typedef struct { uint16_t scr, ink, alarm; uint8_t face; } clock_theme_t;

static const clock_theme_t THEMES[] = {
    { C565(0x05,0x05,0x06), C565(0xee,0xf1,0xee), C565(0x33,0xd3,0xc9), FACE_SEG7  }, /* Midnight */
    { C565(0x0a,0x08,0x06), C565(0xff,0xb6,0x38), C565(0xff,0xd9,0x8a), FACE_PIXEL }, /* Amber    */
    { C565(0x0c,0x13,0x0b), C565(0x8f,0xe3,0x6a), C565(0xc8,0xf5,0x9a), FACE_SEG7  }, /* Green LCD*/
    { C565(0x08,0x08,0x08), C565(0xf2,0xed,0xe0), C565(0xe0,0xa9,0x4f), FACE_DOT   }, /* Ivory    */
    { C565(0x12,0x0a,0x07), C565(0xff,0x7a,0x3c), C565(0xff,0xd0,0xa0), FACE_PIXEL }, /* Ember    */
    { C565(0x04,0x12,0x1a), C565(0x4f,0xd6,0xe6), C565(0xa0,0xf0,0xff), FACE_DOT   }, /* Aqua     */
    { C565(0x0a,0x06,0x14), C565(0xc7,0x7d,0xff), C565(0xff,0x7a,0xc8), FACE_SEG7  }, /* Neon     */
    { C565(0x0d,0x0f,0x12), C565(0xdf,0xe6,0xee), C565(0x8f,0xb4,0xd8), FACE_PIXEL }, /* Slate    */
};
#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

/* ---- 7-segment vector digit ------------------------------------------- */

static const uint8_t SEG7[10] = { 0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F };

static void draw_seg_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9) return;
    uint8_t m = SEG7[d];
    int vlen = (h - 3 * t) / 2, rx = x + w - t, by = y + h - t, my = y + t + vlen, ey = y + 2 * t + vlen;
    if (m & 0x01) odroid_overlay_draw_fill_rect(x + t, y,   w - 2*t, t, col);
    if (m & 0x02) odroid_overlay_draw_fill_rect(rx,    y + t, t, vlen, col);
    if (m & 0x04) odroid_overlay_draw_fill_rect(rx,    ey,   t, vlen, col);
    if (m & 0x08) odroid_overlay_draw_fill_rect(x + t, by,   w - 2*t, t, col);
    if (m & 0x10) odroid_overlay_draw_fill_rect(x,     ey,   t, vlen, col);
    if (m & 0x20) odroid_overlay_draw_fill_rect(x,     y + t, t, vlen, col);
    if (m & 0x40) odroid_overlay_draw_fill_rect(x + t, my,   w - 2*t, t, col);
}

/* ---- 5x7 pixel/dot digit ---------------------------------------------- */

static const uint8_t DOT5x7[10][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
};

static void draw_pix_digit(int d, int x, int y, int px, uint16_t col, bool dot)
{
    if (d < 0 || d > 9) return;
    int inset = dot ? 1 : 0, sz = px - (dot ? 2 : 0);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (DOT5x7[d][r] & (1 << (4 - c)))
                odroid_overlay_draw_fill_rect(x + c*px + inset, y + r*px + inset, sz, sz, col);
}

/* Width of a "HH:MM" block for each face, so we can centre it. */
static int big_time_width(digit_face_t face)
{
    if (face == FACE_SEG7) { int w=44,t=10,gap=10; return 4*w + 3*gap + (t+2*gap); }
    int px = 9; return 4*(5*px) + 3*px + (px*3);   /* pixel/dot: 5*px per digit, gaps, colon */
}

/* Draw "HH:MM" centred; colon blinks when colon=false it's hidden. */
static void draw_big_time(int hh, int mm, bool colon, digit_face_t face, uint16_t col)
{
    int total = big_time_width(face);
    int x = (GW_LCD_WIDTH - total) / 2;
    int a = hh/10, b = hh%10, c = mm/10, e = mm%10;

    if (face == FACE_SEG7) {
        int w=44,h=92,t=10,gap=10,y=58;
        draw_seg_digit(a, x, y, w, h, t, col); x += w+gap;
        draw_seg_digit(b, x, y, w, h, t, col); x += w+gap;
        if (colon) { odroid_overlay_draw_fill_rect(x+gap, y+h/3, t, t, col);
                     odroid_overlay_draw_fill_rect(x+gap, y+2*h/3, t, t, col); }
        x += t+2*gap;
        draw_seg_digit(c, x, y, w, h, t, col); x += w+gap;
        draw_seg_digit(e, x, y, w, h, t, col);
    } else {
        bool dot = (face == FACE_DOT);
        int px=9, dw=5*px, y=64;
        draw_pix_digit(a, x, y, px, col, dot); x += dw+px;
        draw_pix_digit(b, x, y, px, col, dot); x += dw+px;
        if (colon) { odroid_overlay_draw_fill_rect(x+px, y+2*px, px, px, col);
                     odroid_overlay_draw_fill_rect(x+px, y+4*px, px, px, col); }
        x += px*3;
        draw_pix_digit(c, x, y, px, col, dot); x += dw+px;
        draw_pix_digit(e, x, y, px, col, dot);
    }
}

/* ---- small glyphs (DND moon) ------------------------------------------ */

static void fill_disc(int cx, int cy, int r, uint16_t col)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0; while (dx*dx + dy*dy <= r*r) dx++;
        if (dx) odroid_overlay_draw_fill_rect(cx - dx + 1, cy + dy, 2*dx - 1, 1, col);
    }
}

/* Crescent: a disc with an offset disc punched out in the background colour. */
static void draw_moon(int x, int y, uint16_t col, uint16_t bg)
{
    fill_disc(x + 6, y + 6, 6, col);
    fill_disc(x + 9, y + 5, 6, bg);
}

/* ---- i18n text: centred line (CJK-aware width estimate) ---------------- */

static int i18n_text_w(const char *s)
{
    int w = 0;
    while (*s) { unsigned char c = *s;
        if (c < 0x80) { w += 6; s += 1; }
        else if (c < 0xE0) { w += 6; s += 2; }
        else { w += 12; s += 3; } }
    return w;
}

static void draw_centered_i18n(int y, const char *text, uint16_t col)
{
    int x = (GW_LCD_WIDTH - i18n_text_w(text)) / 2;
    if (x < 0) x = 0;
    i18n_draw_text_line(x, y, GW_LCD_WIDTH - x, text, col, CLOCK_BLACK, 1);
}

/* ---- config + alarms (/clock.cfg) ------------------------------------- */

#define CLOCK_CFG_PATH  "/clock.cfg"
#define MAX_ALARMS      8

typedef struct { uint8_t hour, min, enabled; } alarm_t;

static int      s_theme;
static bool     s_hour24;
static bool     s_dnd;
static alarm_t  s_alarms[MAX_ALARMS];
static int      s_alarm_count;

static void clock_config_load(void)
{
    s_theme = 0; s_hour24 = false; s_dnd = false; s_alarm_count = 0;
    FILE *f = fopen(CLOCK_CFG_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "theme=%d", &v) == 1) { if (v >= 0 && v < THEME_COUNT) s_theme = v; }
        else if (sscanf(line, "hour24=%d", &v) == 1) s_hour24 = v != 0;
        else if (sscanf(line, "dnd=%d", &v) == 1) s_dnd = v != 0;
        else if (sscanf(line, "alarm=%d", &v) == 1 && s_alarm_count < MAX_ALARMS) {
            s_alarms[s_alarm_count].hour = (v / 100) % 24;
            s_alarms[s_alarm_count].min  = v % 100;
            s_alarms[s_alarm_count].enabled = 1;
            s_alarm_count++;
        }
    }
    fclose(f);
}

static void clock_config_save(void)
{
    FILE *f = fopen(CLOCK_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", s_theme);
    fprintf(f, "hour24=%d\n", s_hour24 ? 1 : 0);
    fprintf(f, "dnd=%d\n", s_dnd ? 1 : 0);
    for (int i = 0; i < s_alarm_count; i++)
        if (s_alarms[i].enabled)
            fprintf(f, "alarm=%02d%02d\n", s_alarms[i].hour, s_alarms[i].min);
    fclose(f);
}

/* Minutes-from-now to the soonest enabled alarm; -1 if none. Fills *idx. */
static int next_alarm(int now_h, int now_m, int *idx)
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

/* ---- runners (pomodoro / timer / stopwatch) --------------------------- */

typedef enum { MODE_CLOCK = 0, MODE_POMODORO, MODE_TIMER, MODE_STOPWATCH, MODE_COUNT } clock_mode_t;
static const char *const MODE_NAME[MODE_COUNT] = { "CLOCK", "POMODORO", "TIMER", "STOPWATCH" };

typedef enum { RUN_STOPPED = 0, RUN_RUNNING, RUN_PAUSED } run_state_t;
typedef struct { run_state_t state; uint32_t remaining_ms, elapsed_ms, last_tick; } runner_t;

static runner_t s_timer = { RUN_STOPPED, 5*60*1000, 0, 0 };
static runner_t s_watch = { RUN_STOPPED, 0, 0, 0 };
static int  s_pomo_work_min = 25, s_pomo_break_min = 5, s_pomo_cycles = 0;
static bool s_pomo_on_break = false;
static runner_t s_pomo = { RUN_STOPPED, 25*60*1000, 0, 0 };
static uint32_t s_flash_until = 0;

static void tick_countdown(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING) return;
    uint32_t dt = now - r->last_tick; r->last_tick = now;
    if (dt >= r->remaining_ms) { r->remaining_ms = 0; r->state = RUN_STOPPED; }
    else r->remaining_ms -= dt;
}
static void tick_countup(runner_t *r, uint32_t now)
{
    if (r->state != RUN_RUNNING) return;
    r->elapsed_ms += now - r->last_tick; r->last_tick = now;
}
static void runner_toggle(runner_t *r, uint32_t now)
{
    if (r->state == RUN_RUNNING) r->state = RUN_PAUSED;
    else { r->state = RUN_RUNNING; r->last_tick = now; }
}

/* ---- rendering -------------------------------------------------------- */

static const clock_theme_t *TH(void) { return &THEMES[s_theme]; }

static void draw_header(clock_mode_t mode, uint16_t col)
{
    char line[40]; snprintf(line, sizeof line, "< %s >", MODE_NAME[mode]);
    int x = (GW_LCD_WIDTH - (int)strlen(line) * odroid_overlay_get_font_width()) / 2;
    odroid_overlay_draw_text(x, 6, GW_LCD_WIDTH - x, line, col, CLOCK_BLACK);
}

static void draw_hint(const char *hint, uint16_t col)
{
    int x = (GW_LCD_WIDTH - (int)strlen(hint) * odroid_overlay_get_font_width()) / 2;
    if (x < 0) x = 0;
    odroid_overlay_draw_text(x, GW_LCD_HEIGHT - 14, GW_LCD_WIDTH - x, hint, col, CLOCK_BLACK);
}

static const char *weekday_str(void)
{
    int wd = GW_GetCurrentWeekday(); if (wd < 1 || wd > 7) wd = 1;
    const char *w[7] = { curr_lang->s_Weekday_Mon, curr_lang->s_Weekday_Tue, curr_lang->s_Weekday_Wed,
                         curr_lang->s_Weekday_Thu, curr_lang->s_Weekday_Fri, curr_lang->s_Weekday_Sat,
                         curr_lang->s_Weekday_Sun };
    return w[wd - 1];
}

static void render_clock(bool alarm_firing)
{
    const clock_theme_t *t = TH();
    int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();
    bool colon = GW_GetCurrentSubSeconds() <= 127;

    /* logo (retro-go G&W, reused as-is) + battery + DND, one colour */
    odroid_overlay_draw_logo(9, 8, RG_LOGO_GNW, t->ink);
    odroid_overlay_draw_battery(odroid_input_read_battery(), GW_LCD_WIDTH - 26, 11);
    if (s_dnd) draw_moon(GW_LCD_WIDTH - 48, 9, t->ink, t->scr);

    /* date + weekday, centred, in the normal i18n UI font */
    char date[48];
    snprintf(date, sizeof date, "%02d/%02d %s", GW_GetCurrentMonth(), GW_GetCurrentDay(), weekday_str());
    draw_centered_i18n(11, date, t->ink);

    /* big time */
    int dh = hh;
    if (!s_hour24) { dh = hh % 12; if (dh == 0) dh = 12; }
    draw_big_time(dh, mm, colon, t->face, t->ink);

    /* AM/PM on its own centred line (i18n, normal font — digit faces have no letters) */
    if (!s_hour24)
        draw_centered_i18n(158, hh < 12 ? curr_lang->s_AM : curr_lang->s_PM, t->ink);

    /* next alarm */
    if (s_alarm_count > 0) {
        int idx, mins = next_alarm(hh, mm, &idx);
        if (mins >= 0) {
            char al[48]; int ah = s_alarms[idx].hour, am = s_alarms[idx].min;
            int enabled = 0; for (int i = 0; i < s_alarm_count; i++) enabled += s_alarms[i].enabled;
            if (s_hour24) snprintf(al, sizeof al, "%02d:%02d", ah, am);
            else { int h12 = ah % 12; if (h12 == 0) h12 = 12;
                   snprintf(al, sizeof al, "%s %d:%02d", ah < 12 ? curr_lang->s_AM : curr_lang->s_PM, h12, am); }
            char line[64];
            if (enabled > 1) snprintf(line, sizeof line, "* %s  +%d", al, enabled - 1);
            else snprintf(line, sizeof line, "* %s", al);
            draw_centered_i18n(GW_LCD_HEIGHT - 32, line, alarm_firing ? t->ink : t->alarm);
        }
    }
    draw_hint("A exit   L/R mode   UP/DN theme", 0x8410 /* dim grey */);
}

static void render_mmss(uint32_t ms, uint16_t col, bool colon)
{
    uint32_t total = ms / 1000; int m = (total / 60) % 100, s = total % 60;
    draw_big_time(m, s, colon, TH()->face, col);
}

static void render_pomodoro(uint32_t now)
{
    const clock_theme_t *t = TH();
    tick_countdown(&s_pomo, now);
    if (s_pomo.state == RUN_STOPPED && s_pomo.remaining_ms == 0) {
        s_flash_until = now + 800; s_pomo_on_break = !s_pomo_on_break;
        if (!s_pomo_on_break) s_pomo_cycles++;
        s_pomo.remaining_ms = (s_pomo_on_break ? s_pomo_break_min : s_pomo_work_min) * 60u*1000u;
        s_pomo.state = RUN_RUNNING; s_pomo.last_tick = now;
    }
    bool colon = (now / 500) & 1;
    render_mmss(s_pomo.remaining_ms, s_pomo_on_break ? t->alarm : t->ink,
                s_pomo.state != RUN_RUNNING ? true : colon);
    char st[48]; snprintf(st, sizeof st, "%s   cycle %d", s_pomo_on_break ? "BREAK" : "WORK", s_pomo_cycles + 1);
    draw_centered_i18n(178, st, t->ink);
    draw_hint(s_pomo.state == RUN_RUNNING ? "A pause  SELECT reset  B exit"
                                          : "A start  UP/DN work min  B exit", 0x8410);
}

static void render_timer(uint32_t now)
{
    tick_countdown(&s_timer, now);
    if (s_timer.state == RUN_STOPPED && s_timer.remaining_ms == 0 && s_flash_until < now)
        s_flash_until = now + 800;
    bool colon = (now / 500) & 1;
    render_mmss(s_timer.remaining_ms, TH()->ink, s_timer.state != RUN_RUNNING ? true : colon);
    draw_hint(s_timer.state == RUN_RUNNING ? "A pause  SELECT reset  B exit"
                                           : "A start  UP/DN +/-1 min  B exit", 0x8410);
}

static void render_stopwatch(uint32_t now)
{
    tick_countup(&s_watch, now);
    render_mmss(s_watch.elapsed_ms, TH()->ink, true);
    char cs[16]; snprintf(cs, sizeof cs, ".%02u", (unsigned)((s_watch.elapsed_ms / 10) % 100));
    draw_centered_i18n(178, cs, TH()->alarm);
    draw_hint(s_watch.state == RUN_RUNNING ? "A pause  SELECT reset  B exit"
                                           : "A start  SELECT reset  B exit", 0x8410);
}

/* ---- input ------------------------------------------------------------ */

static bool pressed(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, int key)
{ return k->values[key] && !p->values[key]; }

static void input_pomodoro(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) {
        if (s_pomo.state == RUN_STOPPED) { s_pomo_on_break = false; s_pomo.remaining_ms = s_pomo_work_min*60u*1000u; }
        runner_toggle(&s_pomo, now);
    }
    if (pressed(k, p, ODROID_INPUT_SELECT)) {
        s_pomo.state = RUN_STOPPED; s_pomo_on_break = false; s_pomo_cycles = 0;
        s_pomo.remaining_ms = s_pomo_work_min*60u*1000u;
    }
    if (s_pomo.state != RUN_RUNNING) {
        if (pressed(k, p, ODROID_INPUT_UP)   && s_pomo_work_min < 90) s_pomo.remaining_ms = (++s_pomo_work_min)*60u*1000u;
        if (pressed(k, p, ODROID_INPUT_DOWN) && s_pomo_work_min > 1)  s_pomo.remaining_ms = (--s_pomo_work_min)*60u*1000u;
    }
}

static void input_timer(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_timer, now);
    if (pressed(k, p, ODROID_INPUT_SELECT)) { s_timer.state = RUN_STOPPED; s_timer.remaining_ms = 5*60*1000; }
    if (s_timer.state != RUN_RUNNING) {
        if (pressed(k, p, ODROID_INPUT_UP)) s_timer.remaining_ms += 60*1000;
        if (pressed(k, p, ODROID_INPUT_DOWN) && s_timer.remaining_ms >= 60*1000) s_timer.remaining_ms -= 60*1000;
    }
}

static void input_stopwatch(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_watch, now);
    if (pressed(k, p, ODROID_INPUT_SELECT)) { s_watch.state = RUN_STOPPED; s_watch.elapsed_ms = 0; }
}

/* ---- alarm firing (while awake) --------------------------------------- */

static int s_last_fired_min = -1;   /* minute-of-day we last fired, avoids re-fire */

static bool alarm_should_fire(int hh, int mm)
{
    if (s_dnd) return false;
    int mod = hh * 60 + mm;
    if (mod == s_last_fired_min) return false;
    for (int i = 0; i < s_alarm_count; i++)
        if (s_alarms[i].enabled && s_alarms[i].hour == hh && s_alarms[i].min == mm) {
            s_last_fired_min = mod; return true;
        }
    return false;
}

/* ---- main loop -------------------------------------------------------- */

void rg_clock_show(void)
{
    clock_mode_t mode = MODE_CLOCK;
    odroid_gamepad_state_t k, prev = {0};
    uint32_t alarm_ring_until = 0;
    bool dirty = true;

    clock_config_load();
    odroid_input_read_gamepad(&prev);   /* swallow the opening button */

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();

        bool exit_app = k.values[ODROID_INPUT_POWER];
        if (mode == MODE_CLOCK) exit_app |= pressed(&k, &prev, ODROID_INPUT_A);
        else                    exit_app |= pressed(&k, &prev, ODROID_INPUT_B);
        if (exit_app) break;

        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { mode = (mode == 0) ? MODE_COUNT-1 : mode-1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { mode = (mode+1) % MODE_COUNT; dirty = true; }

        if (mode == MODE_CLOCK) {
            /* Config is written once on exit — not per keypress — so cycling
             * themes never blocks on an SD write. */
            if (pressed(&k, &prev, ODROID_INPUT_UP))   { s_theme = (s_theme+1) % THEME_COUNT; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { s_theme = (s_theme == 0) ? THEME_COUNT-1 : s_theme-1; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_SELECT)) { s_hour24 = !s_hour24; dirty = true; }
        } else {
            switch (mode) {
            case MODE_POMODORO:  input_pomodoro(&k, &prev, now);  break;
            case MODE_TIMER:     input_timer(&k, &prev, now);     break;
            case MODE_STOPWATCH: input_stopwatch(&k, &prev, now); break;
            default: break;
            }
        }

        /* Alarm check (clock time). Ring = flash for 20s or until a key. */
        int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();
        if (alarm_should_fire(hh, mm)) alarm_ring_until = now + 20000;
        if (alarm_ring_until > now && k.bitmask) alarm_ring_until = 0;   /* dismiss */
        bool ringing = alarm_ring_until > now;

        /* Only repaint when the visible frame actually changes — saves battery.
         * The signature captures everything on screen for the current mode. */
        static uint32_t last_sig = 0xFFFFFFFF;
        uint32_t sig;
        if (mode == MODE_CLOCK)
            sig = (1u<<30) | (hh<<20) | (mm<<12) | ((GW_GetCurrentSubSeconds() <= 127)<<11)
                | (s_theme<<7) | (s_hour24<<6) | (s_dnd<<5) | (ringing<<4);
        else if (mode == MODE_STOPWATCH)
            sig = (2u<<30) | (s_watch.elapsed_ms / 10);           /* centiseconds */
        else {
            runner_t *r = (mode == MODE_TIMER) ? &s_timer : &s_pomo;
            sig = ((uint32_t)mode<<28) | (r->remaining_ms/500) | (s_pomo_on_break<<27);
        }
        bool flash = (s_flash_until > now || ringing) && ((now/150) & 1);
        if (flash) sig ^= 0x55555555;

        if (dirty || sig != last_sig) {
            last_sig = sig; dirty = false;
            uint16_t *fb = lcd_get_active_buffer();
            uint16_t bg = flash ? TH()->ink : TH()->scr;
            for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
            draw_header(mode, TH()->alarm);
            switch (mode) {
            case MODE_CLOCK:     render_clock(ringing); break;
            case MODE_POMODORO:  render_pomodoro(now);  break;
            case MODE_TIMER:     render_timer(now);     break;
            case MODE_STOPWATCH: render_stopwatch(now); break;
            default: break;
            }
            lcd_swap();
            lcd_sleep_while_swap_pending();
        }

        prev = k;
        HAL_Delay(mode == MODE_STOPWATCH ? 20 : 40);   /* low-power idle between frames */
    }

    clock_config_save();
}
