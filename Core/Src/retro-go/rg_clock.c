/* Full-screen Clock app — see rg_clock.h.
 *
 * A mode switcher (Left/Right) over one shared layout: top bar (G&W logo,
 * "< MODE >" title, pager dots, battery), big 7-seg time with LCD ghost
 * segments, one status row, and an always-on rounded hint strip (8px default
 * font) — clock, Pomodoro,
 * countdown timer and stopwatch all read as one app. ONE fixed look (no theme
 * or font pickers); the customisable part is the background (off / ambient /
 * user GIF via /clock/bg.gif). Every label comes from the firmware i18n
 * table (hint legends stay ASCII for the 8px font). Controls are uniform:
 * A start/pause, B reset, PAUSE = settings (incl. Exit), POWER exits; while
 * the alarm rings A = snooze (5 min), anything else stops it. Alarm loudness
 * follows the SYSTEM volume. Config (24h, DND, alarms) = /clock.cfg.
 *
 * Runs inside the launcher context (no APPID overlay), so it costs a handful
 * of bytes of RAM and can never reduce an emulator's heap or DTCM. The paint
 * loop only redraws when the visible frame changes, to spare the battery;
 * host/clock_preview.c renders these exact draw calls to PNGs on a PC. */

#include <odroid_system.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "rg_rtc.h"
#include "rg_i18n.h"
#include "gui.h"
#include "bitmaps.h"
#include "odroid_overlay.h"
#include "odroid_input.h"
#include "odroid_audio.h"
#include "common.h"   /* volume_tbl — alarm loudness = the SYSTEM volume */
#include "rg_clock.h"
#include "rg_clock_gif.h"

#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define CLOCK_BLACK 0x0000

/* ---- the ONE look ------------------------------------------------------
 * Single fixed palette + single digit face (7-seg with LCD ghost segments),
 * per the design brief: no theme picker, no font picker — the customisable
 * part is the BACKGROUND (off / ambient / user GIF). All text uses the one
 * firmware i18n font. */

typedef struct { uint16_t scr, ink, alarm; } clock_theme_t;

static const clock_theme_t THEME =
    { C565(0x07,0x0a,0x10), C565(0xee,0xf1,0xee), C565(0x33,0xd3,0xc9) };

/* ---- 7-segment vector digit --------------------------------------------
 * Real-LCD styling: every segment is a HEXAGON (ends taper to a point at
 * 45°) and the whole digit leans right by SEG_SLANT px (italic), the way
 * DSEG-style clock faces do — not seven butted rectangles. Drawn as 1px
 * rows so the shear is exact; still just fill_rect calls underneath. */

#define SEG_SLANT 5   /* total rightward lean, top row vs bottom row */

static const uint8_t SEG7[10] = { 0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F };

static int seg_shear(int y_rel, int h) { return (SEG_SLANT * (h - y_rel)) / h; }

static void draw_seg_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9) return;
    uint8_t m = SEG7[d];
    int vlen = (h - 3 * t) / 2;
    /* segment boxes, unsheared: bit order A,B,C,D,E,F,G */
    const struct { int16_t x0, y0, len; uint8_t vert; } S[7] = {
        { (int16_t)t,       0,                     (int16_t)(w - 2*t), 0 },  /* A */
        { (int16_t)(w - t), (int16_t)t,            (int16_t)vlen,      1 },  /* B */
        { (int16_t)(w - t), (int16_t)(2*t + vlen), (int16_t)vlen,      1 },  /* C */
        { (int16_t)t,       (int16_t)(h - t),      (int16_t)(w - 2*t), 0 },  /* D */
        { 0,                (int16_t)(2*t + vlen), (int16_t)vlen,      1 },  /* E */
        { 0,                (int16_t)t,            (int16_t)vlen,      1 },  /* F */
        { (int16_t)t,       (int16_t)(t + vlen),   (int16_t)(w - 2*t), 0 },  /* G */
    };
    for (int s = 0; s < 7; s++) {
        if (!(m & (1u << s))) continue;
        if (!S[s].vert) {
            for (int r = 0; r < t; r++) {           /* horizontal hexagon */
                int c2 = 2*r - (t - 1), dc = c2 < 0 ? -c2 : c2;
                int inset = (dc + 1) / 2 + 1;
                int yr = S[s].y0 + r;
                odroid_overlay_draw_fill_rect(x + S[s].x0 + inset + seg_shear(yr, h),
                                              y + yr, S[s].len - 2*inset, 1, col);
            }
        } else {
            for (int r = 0; r < S[s].len; r++) {    /* vertical hexagon */
                int e = r < S[s].len - 1 - r ? r : S[s].len - 1 - r;
                int wid = 2*e + 1; if (wid > t - 1) wid = t - 1;
                int yr = S[s].y0 + r;
                odroid_overlay_draw_fill_rect(x + S[s].x0 + (t - wid)/2 + seg_shear(yr, h),
                                              y + yr, wid, 1, col);
            }
        }
    }
}

/* Geometry of the one big "HH:MM" block (7-seg, centred). */
#define SEG_W    44
#define SEG_H    92
#define SEG_T    10
#define SEG_GAP  10
#define SEG_Y    58
#define BIG_TIME_W (4*SEG_W + 3*SEG_GAP + (SEG_T + 2*SEG_GAP) + SEG_SLANT)

/* Draw "HH:MM" centred; when colon=false the colon drops to the ghost shade.
 * Every segment is first drawn in a faint "ghost" colour, the lit ones on
 * top — the unlit-segment look of a real LCD alarm clock. blank_lead hides a
 * leading zero the way segment clocks do (12h "9:41", not "09:41"). */
static void draw_big_time(int hh, int mm, bool colon, bool blank_lead,
                          uint16_t col, uint16_t ghost)
{
    int x = (GW_LCD_WIDTH - BIG_TIME_W) / 2, y = SEG_Y;
    int w = SEG_W, h = SEG_H, t = SEG_T, gap = SEG_GAP;
    int a = hh/10, b = hh%10, c = mm/10, e = mm%10;

    draw_seg_digit(8, x, y, w, h, t, ghost);   /* digit 8 lights all segments */
    if (!(blank_lead && a == 0))
        draw_seg_digit(a, x, y, w, h, t, col);
    x += w+gap;
    draw_seg_digit(8, x, y, w, h, t, ghost);
    draw_seg_digit(b, x, y, w, h, t, col); x += w+gap;
    uint16_t cc = colon ? col : ghost;
    odroid_overlay_draw_fill_rect(x+gap, y+h/3, t, t, cc);
    odroid_overlay_draw_fill_rect(x+gap, y+2*h/3, t, t, cc);
    x += t+2*gap;
    draw_seg_digit(8, x, y, w, h, t, ghost);
    draw_seg_digit(c, x, y, w, h, t, col); x += w+gap;
    draw_seg_digit(8, x, y, w, h, t, ghost);
    draw_seg_digit(e, x, y, w, h, t, col);
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

/* Tiny bell — shown whenever at least one alarm is armed. */
static void draw_bell(int x, int y, uint16_t col)
{
    fill_disc(x + 5, y + 4, 3, col);                          /* dome */
    odroid_overlay_draw_fill_rect(x + 2, y + 4, 7, 3, col);   /* body */
    odroid_overlay_draw_fill_rect(x + 1, y + 7, 9, 1, col);   /* flared lip */
    odroid_overlay_draw_fill_rect(x + 4, y + 9, 3, 1, col);   /* clapper */
}

/* Ambient "low battery" animation: a few twinkling dots, ~3 fps. Fixed pseudo-
 * random positions clear of the top bar and hint bar; each dot pulses on a
 * staggered phase. Cheap (no assets, no SD) — it only bumps the repaint rate,
 * which is why its cost is "low". */
static void draw_ambient(uint32_t now, uint16_t col)
{
    uint16_t dim = (col >> 1) & 0x7BEF;
    uint32_t ph = now / 320;
    for (int i = 0; i < 26; i++) {
        int x = (i*73 + 11) % GW_LCD_WIDTH;
        int y = 34 + (i*127 + 7) % (GW_LCD_HEIGHT - 34 - 34);
        int p = (ph + i*3) % 9;
        if (p < 3) { int sz = (p == 1) ? 2 : 1; odroid_overlay_draw_fill_rect(x, y, sz, sz, p == 1 ? col : dim); }
    }
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

static bool     s_hour24;
static bool     s_dnd;
static int      s_anim;          /* 0 = off, 1 = ambient, 2 = GIF */
static alarm_t  s_alarms[MAX_ALARMS];
static int      s_alarm_count;

static int alarms_armed(void)
{
    int n = 0;
    for (int i = 0; i < s_alarm_count; i++) n += s_alarms[i].enabled;
    return n;
}

/* Background levels — THE one customisable visual. Each is labelled with its
 * battery cost so the choice is informed: "off" keeps the fully event-driven,
 * near-zero-draw loop; "ambient" (procedural twinkling dots) only bumps the
 * repaint rate to ~3 fps; "GIF" (/clock/bg.gif, decoded on the fly — see
 * rg_clock_gif) repaints the whole face at the GIF rate, hence "high". */
#define ANIM_COUNT 3
#define ANIM_GIF   2

static void clock_config_load(void)
{
    s_hour24 = false; s_dnd = false; s_anim = 0;
    s_alarm_count = 0;
    FILE *f = fopen(CLOCK_CFG_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v, en = 1;
        /* legacy theme=/face= lines are ignored (single look now) */
        if (sscanf(line, "hour24=%d", &v) == 1) s_hour24 = v != 0;
        else if (sscanf(line, "dnd=%d", &v) == 1) s_dnd = v != 0;
        else if (sscanf(line, "anim=%d", &v) == 1) { if (v >= 0 && v < ANIM_COUNT) s_anim = v; }
        /* alarm=HHMM[,enabled] — the suffix is new; plain HHMM (older cfg) = enabled */
        else if (sscanf(line, "alarm=%d,%d", &v, &en) >= 1 && s_alarm_count < MAX_ALARMS) {
            int hr = v / 100, mn = v % 100;
            if (v < 0 || hr > 23 || mn > 59) continue;   /* reject hand-edited junk */
            s_alarms[s_alarm_count].hour = hr;
            s_alarms[s_alarm_count].min  = mn;
            s_alarms[s_alarm_count].enabled = en ? 1 : 0;
            s_alarm_count++;
        }
    }
    fclose(f);
}

static void clock_config_save(void)
{
    FILE *f = fopen(CLOCK_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "hour24=%d\n", s_hour24 ? 1 : 0);
    fprintf(f, "dnd=%d\n", s_dnd ? 1 : 0);
    fprintf(f, "anim=%d\n", s_anim);
    for (int i = 0; i < s_alarm_count; i++)   /* disabled alarms persist too */
        fprintf(f, "alarm=%02d%02d,%d\n", s_alarms[i].hour, s_alarms[i].min,
                s_alarms[i].enabled ? 1 : 0);
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

typedef enum { RUN_STOPPED = 0, RUN_RUNNING, RUN_PAUSED } run_state_t;
typedef struct { run_state_t state; uint32_t remaining_ms, elapsed_ms, last_tick; } runner_t;

static runner_t s_timer = { RUN_STOPPED, 5*60*1000, 0, 0 };
static runner_t s_watch = { RUN_STOPPED, 0, 0, 0 };
static int  s_pomo_work_min = 25, s_pomo_break_min = 5, s_pomo_cycles = 0;
static bool s_pomo_on_break = false;
static runner_t s_pomo = { RUN_STOPPED, 25*60*1000, 0, 0 };
static uint32_t s_flash_until = 0;
#define SNOOZE_MS (5u * 60u * 1000u)
static uint32_t s_snooze_tick = 0;   /* HAL tick when a snoozed alarm re-rings */

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

static const clock_theme_t *TH(void) { return &THEME; }

#define STATUS_Y 178   /* the one shared status-line row under the digits */

/* blend a toward b by t/16 per RGB565 channel */
static uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    int ar = (a>>11)&31, ag = (a>>5)&63, ab = a&31;
    int br = (b>>11)&31, bg = (b>>5)&63, bb = b&31;
    return (uint16_t)(((ar+(br-ar)*t/16)<<11) | ((ag+(bg-ag)*t/16)<<5) | (ab+(bb-ab)*t/16));
}

static const char *mode_title(clock_mode_t mode)
{
    switch (mode) {
    case MODE_POMODORO:  return curr_lang->s_Clock_Pomodoro;
    case MODE_TIMER:     return curr_lang->s_Clock_Timer;
    case MODE_STOPWATCH: return curr_lang->s_Clock_Stopwatch;
    default:             return curr_lang->s_Clock;
    }
}

/* One SHARED top bar for every mode — logo, "< MODE >" (i18n, transparent, no
 * black box), mode pager dots, battery (+DND moon) — so the four screens read
 * as one app. The date lives BELOW it (render_clock), not on top of it. */
static void draw_topbar(clock_mode_t mode)
{
    const clock_theme_t *t = TH();
    odroid_overlay_draw_logo(9, 8, RG_LOGO_GNW, t->ink);
    odroid_overlay_draw_battery(odroid_input_read_battery(), GW_LCD_WIDTH - 26, 11);
    if (s_dnd) draw_moon(GW_LCD_WIDTH - 48, 9, t->ink, t->scr);
    if (alarms_armed()) draw_bell(GW_LCD_WIDTH - (s_dnd ? 66 : 48), 10, t->alarm);

    char line[48]; snprintf(line, sizeof line, "< %s >", mode_title(mode));
    draw_centered_i18n(6, line, t->alarm);

    /* pager dots: which of the four modes you are on */
    int dx = (GW_LCD_WIDTH - (MODE_COUNT*8 - 4)) / 2;
    for (int i = 0; i < MODE_COUNT; i++, dx += 8)
        odroid_overlay_draw_fill_rect(dx, 22, 4, 4,
            i == (int)mode ? t->alarm : mix565(t->scr, t->ink, 4));
}

/* Bottom hint: a quiet theme-tinted panel, identical in every mode. */
/* Bottom hint: ALWAYS visible, in the firmware's default 8px font (crisp,
 * matches the rest of the OS chrome — the 12px serif looked broken here),
 * sitting on a rounded pill so it reads as one quiet control strip. */
static void draw_round_panel(int x, int y, int w, int h, int r, uint16_t col)
{
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? r - 1 - j : (j >= h - r ? j - (h - r) : -1);
        int inset = 0;
        if (dy >= 0) { inset = r; for (int k = 0; k <= r; k++) if (k*k + dy*dy <= r*r) { inset = r - k; break; } }
        odroid_overlay_draw_fill_rect(x + inset, y + j, w - 2*inset, 1, col);
    }
}

static void draw_hintbar(const char *hint)
{
    const clock_theme_t *t = TH();
    uint16_t panel = mix565(t->scr, t->ink, 2);
    uint16_t txt   = mix565(t->scr, t->ink, 9);
    int w = (int)strlen(hint) * odroid_overlay_get_font_width();
    int x = (GW_LCD_WIDTH - w) / 2, y = GW_LCD_HEIGHT - 22;
    if (x < 4) x = 4;
    draw_round_panel(x - 12, y - 4, w + 24, 16, 7, panel);
    /* width must be EXACTLY the text width: draw_text paints the glyph-cell
     * background across the whole width you hand it (the old full-screen
     * width here is what smeared a dark band across the face) */
    odroid_overlay_draw_text(x, y, w, hint, txt, panel);
}

/* Hint legends are fixed ASCII (button names are Latin on the shell anyway)
 * so the 8px font can render them in every language. */
#define HINT_CLOCK       "PAUSE settings"
#define HINT_RUN         "A start/stop   B reset"
#define HINT_TIMER_STOP  "A start/stop   B reset   UP/DN min"
#define HINT_EDITOR      "A edit   TIME on/off   GAME del   B done"
#define HINT_EDIT        "L/R field   UP/DN set   A ok   B cancel"
#define HINT_RINGING     "A snooze 5min   B stop"

static const char *weekday_str(void)
{
    int wd = GW_GetCurrentWeekday(); if (wd < 1 || wd > 7) wd = 1;
    const char *w[7] = { curr_lang->s_Weekday_Mon, curr_lang->s_Weekday_Tue, curr_lang->s_Weekday_Wed,
                         curr_lang->s_Weekday_Thu, curr_lang->s_Weekday_Fri, curr_lang->s_Weekday_Sat,
                         curr_lang->s_Weekday_Sun };
    return w[wd - 1];
}

static void render_clock(uint32_t now, bool alarm_firing)
{
    const clock_theme_t *t = TH();
    int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();
    bool colon = GW_GetCurrentSubSeconds() <= 127;

    /* background layer: a user GIF (level 2) or the procedural ambient (level 1) */
    if (s_anim == ANIM_GIF && clock_gif_ready())
        clock_gif_blit(lcd_get_active_buffer(), now);
    else if (s_anim == 1)
        draw_ambient(now, t->ink);

    /* date + weekday, centred UNDER the top bar (was colliding with it) */
    char date[48];
    snprintf(date, sizeof date, "%02d/%02d %s", GW_GetCurrentMonth(), GW_GetCurrentDay(), weekday_str());
    draw_centered_i18n(32, date, t->ink);

    /* big time. When the alarm is ringing the digits PULSE between ink and
     * the accent (~2.5 Hz): a clear alarm signal that sits on top of the
     * GIF/ambient background instead of a full-screen flash covering it. */
    int dh = hh;
    if (!s_hour24) { dh = hh % 12; if (dh == 0) dh = 12; }
    uint16_t timecol = t->ink;
    if (alarm_firing && ((now / 200) & 1)) timecol = t->alarm;
    draw_big_time(dh, mm, colon, !s_hour24, timecol, mix565(t->scr, t->ink, 2));

    /* AM/PM tucked to the RIGHT of the digits at their baseline (G&W style),
     * not a whole centred line of its own */
    if (!s_hour24) {
        int x = (GW_LCD_WIDTH + BIG_TIME_W) / 2 + 6;
        i18n_draw_text_line(x, SEG_Y + SEG_H - 12, GW_LCD_WIDTH - x,
                            hh < 12 ? curr_lang->s_AM : curr_lang->s_PM, t->ink, CLOCK_BLACK, 1);
    }

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
            if (enabled > 1) snprintf(line, sizeof line, "%s  +%d", al, enabled - 1);
            else snprintf(line, sizeof line, "%s", al);
            /* dimmed under DND — the alarm won't actually ring then */
            uint16_t alcol = s_dnd ? mix565(t->scr, t->ink, 6)
                                   : (alarm_firing ? t->ink : t->alarm);
            int lx = (GW_LCD_WIDTH - i18n_text_w(line)) / 2;
            draw_bell(lx - 16, STATUS_Y, alcol);
            draw_centered_i18n(STATUS_Y, line, alcol);
        }
    }
}

static void render_mmss(uint32_t ms, uint16_t col, bool colon)
{
    uint32_t total = ms / 1000; int m = (total / 60) % 100, s = total % 60;
    draw_big_time(m, s, colon, false, col, mix565(TH()->scr, TH()->ink, 2));
}

/* Runner state advances in the MAIN LOOP (update_* below), never in the
 * render functions: rendering is gated on the frame signature, so a tick that
 * only happens inside render deadlocks — start a stopped runner and nothing
 * changes on screen, hence no repaint, hence no tick, forever. */

static void update_pomodoro(uint32_t now)
{
    tick_countdown(&s_pomo, now);
    if (s_pomo.state == RUN_STOPPED && s_pomo.remaining_ms == 0) {
        s_flash_until = now + 800; s_pomo_on_break = !s_pomo_on_break;
        if (!s_pomo_on_break) s_pomo_cycles++;
        s_pomo.remaining_ms = (s_pomo_on_break ? s_pomo_break_min : s_pomo_work_min) * 60u*1000u;
        s_pomo.state = RUN_RUNNING; s_pomo.last_tick = now;
    }
}

static void update_timer(uint32_t now)
{
    tick_countdown(&s_timer, now);
    if (s_timer.state == RUN_STOPPED && s_timer.remaining_ms == 0 && s_flash_until < now)
        s_flash_until = now + 800;
}

static void render_pomodoro(uint32_t now)
{
    const clock_theme_t *t = TH();
    bool colon = (now / 500) & 1;
    render_mmss(s_pomo.remaining_ms, s_pomo_on_break ? t->alarm : t->ink,
                s_pomo.state != RUN_RUNNING ? true : colon);
    char st[64]; snprintf(st, sizeof st, "%s   %s %d",
        s_pomo_on_break ? curr_lang->s_Clock_Break : curr_lang->s_Clock_Work,
        curr_lang->s_Clock_Cycle, s_pomo_cycles + 1);
    draw_centered_i18n(STATUS_Y, st, s_pomo_on_break ? t->alarm : t->ink);
}

static void render_timer(uint32_t now)
{
    bool colon = (now / 500) & 1;
    render_mmss(s_timer.remaining_ms, TH()->ink, s_timer.state != RUN_RUNNING ? true : colon);
}

static void render_stopwatch(uint32_t now)
{
    bool colon = s_watch.state == RUN_RUNNING ? ((now / 500) & 1) : true;
    render_mmss(s_watch.elapsed_ms, TH()->ink, colon);
}

/* ---- input ------------------------------------------------------------ */

static bool pressed(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, int key)
{ return k->values[key] && !p->values[key]; }

/* Runner controls, SAME in every mode: A = start/pause, B = reset.
 * (Exit lives in the PAUSE menu + POWER — never on a face button.) */
static void input_pomodoro(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
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

static void input_timer(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_timer, now);
    if (pressed(k, p, ODROID_INPUT_B)) { s_timer.state = RUN_STOPPED; s_timer.remaining_ms = 5*60*1000; }
    if (s_timer.state != RUN_RUNNING) {
        if (pressed(k, p, ODROID_INPUT_UP)) s_timer.remaining_ms += 60*1000;
        if (pressed(k, p, ODROID_INPUT_DOWN) && s_timer.remaining_ms >= 60*1000) s_timer.remaining_ms -= 60*1000;
    }
}

static void input_stopwatch(odroid_gamepad_state_t *k, odroid_gamepad_state_t *p, uint32_t now)
{
    if (pressed(k, p, ODROID_INPUT_A)) runner_toggle(&s_watch, now);
    if (pressed(k, p, ODROID_INPUT_B)) { s_watch.state = RUN_STOPPED; s_watch.elapsed_ms = 0; }
}

/* ---- alarm firing (while awake) --------------------------------------- */

static int s_last_fired_min = -1;   /* minute-of-day we last fired, avoids re-fire */

static bool alarm_should_fire(int hh, int mm)
{
    int mod = hh * 60 + mm;
    if (mod != s_last_fired_min) s_last_fired_min = -1;   /* minute passed — re-arm (daily re-fire) */
    if (s_dnd) return false;
    if (mod == s_last_fired_min) return false;
    for (int i = 0; i < s_alarm_count; i++)
        if (s_alarms[i].enabled && s_alarms[i].hour == hh && s_alarms[i].min == mm) {
            s_last_fired_min = mod; return true;
        }
    return false;
}

/* Did any enabled alarm's minute pass while a blocking menu held the loop?
 * Checks (from_mod, to_mod) EXCLUSIVE — to_mod itself is still the current
 * minute and is handled by the regular alarm_should_fire() check. */
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

/* ---- in-app alarm editor (opened from the PAUSE settings menu) --------- */

static void alarm_time_str(char *out, size_t n, int h, int m)
{
    if (s_hour24) snprintf(out, n, "%02d:%02d", h, m);
    else { int h12 = h % 12; if (h12 == 0) h12 = 12;
           snprintf(out, n, "%d:%02d %s", h12, m, h < 12 ? curr_lang->s_AM : curr_lang->s_PM); }
}

/* A proper popup: clean solid base + bordered panel, repainted whole every
 * frame. (The old version re-scrimmed whatever was already on the swap
 * buffer, so each repaint stacked another darkening layer + stale rows.) */
static void render_alarm_setup(int sel, bool editing, int field)
{
    const clock_theme_t *t = TH();
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = t->scr;

    int rows = s_alarm_count + 2, rh = 18;
    int pw = 240, ph = 30 + rows * rh + 10;
    int px = (GW_LCD_WIDTH - pw) / 2, py = (GW_LCD_HEIGHT - 24 - ph) / 2;
    if (py < 6) py = 6;
    odroid_overlay_draw_fill_rect(px - 2, py - 2, pw + 4, ph + 4, mix565(t->scr, t->ink, 6));
    odroid_overlay_draw_fill_rect(px, py, pw, ph, mix565(t->scr, t->ink, 1));

    draw_centered_i18n(py + 6, curr_lang->s_Clock_Alarms, t->alarm);
    int y = py + 30; char line[80];
    for (int i = 0; i < rows; i++, y += rh) {
        bool cur = (i == sel); uint16_t col = cur ? t->ink : mix565(t->scr, t->ink, 8);
        if (cur) odroid_overlay_draw_fill_rect(px + 4, y - 2, pw - 8, rh - 2, mix565(t->scr, t->ink, 3));
        if (i < s_alarm_count) {
            char ts[24]; alarm_time_str(ts, sizeof ts, s_alarms[i].hour, s_alarms[i].min);
            const char *tag = s_alarms[i].enabled ? curr_lang->s_Clock_On : curr_lang->s_Clock_Off;
            if (cur && editing) snprintf(line, sizeof line, "%s  [%s]  %s", ts, field == 0 ? "hh" : "mm", tag);
            else                snprintf(line, sizeof line, "%s        %s", ts, tag);
        } else if (i == s_alarm_count) snprintf(line, sizeof line, "+ %s", curr_lang->s_Clock_Add_Alarm);
        else snprintf(line, sizeof line, "%s", curr_lang->s_Clock_Done);
        draw_centered_i18n(y, line, col);
    }
    draw_hintbar(editing ? HINT_EDIT : HINT_EDITOR);
}

static void alarm_delete_at(int sel)
{
    for (int i = sel; i < s_alarm_count - 1; i++) s_alarms[i] = s_alarms[i+1];
    s_alarm_count--;
}

static void clock_alarm_setup(void)
{
    int sel = 0, field = 0, adding = -1;   /* adding = row index of a not-yet-confirmed add */
    bool editing = false, dirty = true;
    odroid_gamepad_state_t k, prev = {0};
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        int rows = s_alarm_count + 2;

        if (!editing) {
            if (pressed(&k, &prev, ODROID_INPUT_B)) break;
            if (pressed(&k, &prev, ODROID_INPUT_UP))   { sel = (sel == 0) ? rows-1 : sel-1; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { sel = (sel+1) % rows; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_A)) {
                if (sel < s_alarm_count) { editing = true; field = 0; adding = -1; }
                else if (sel == s_alarm_count) {
                    if (s_alarm_count < MAX_ALARMS) { s_alarms[s_alarm_count].hour = 7;
                        s_alarms[s_alarm_count].min = 0; s_alarms[s_alarm_count].enabled = 1;
                        sel = s_alarm_count; s_alarm_count++; editing = true; field = 0; adding = sel; }
                } else break;
                dirty = true;
            }
            else if (sel < s_alarm_count) {   /* exclusive with A: no same-frame edit+delete */
                if (pressed(&k, &prev, ODROID_INPUT_SELECT)) { s_alarms[sel].enabled = !s_alarms[sel].enabled; dirty = true; }
                if (pressed(&k, &prev, ODROID_INPUT_START)) {
                    alarm_delete_at(sel);
                    if (sel >= s_alarm_count && sel > 0) sel--;
                    dirty = true;
                }
            }
        } else {
            alarm_t *a = &s_alarms[sel];
            if (pressed(&k, &prev, ODROID_INPUT_A)) { editing = false; adding = -1; dirty = true; }
            else if (pressed(&k, &prev, ODROID_INPUT_B)) {
                /* B = cancel: a just-added row is removed, not committed */
                if (sel == adding) { alarm_delete_at(sel);
                                     if (sel >= s_alarm_count && sel > 0) sel--; }
                editing = false; adding = -1; dirty = true;
            }
            if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { field = 0; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { field = 1; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_UP))   { if (field == 0) a->hour = (a->hour+1)%24; else a->min = (a->min+1)%60; dirty = true; }
            if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { if (field == 0) a->hour = (a->hour==0)?23:a->hour-1; else a->min = (a->min==0)?59:a->min-1; dirty = true; }
        }

        if (dirty) { dirty = false; render_alarm_setup(sel, editing, field);
                     lcd_swap(); lcd_sleep_while_swap_pending(); }
        prev = k;
        HAL_Delay(40);
    }
    /* wait for release so the closing A/B press can't leak into the caller's
     * loop and exit the whole app (odroid_overlay_dialog does the same) */
    do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); } while (k.bitmask);
    clock_config_save();
    s_last_fired_min = -1;
}

/* ---- settings menu (opened with PAUSE/SET = ODROID_INPUT_VOLUME) --------
 * The proper place to set theme, digit face, format, DND, animation and the
 * alarm volume — a standard dialog like the rest of the firmware, not D-pad
 * shortcuts on the clock face. */

static char v_fmt[8], v_dnd[12], v_anim[40], v_vol[8], v_alarms[4], v_exit[4];

static bool cb_fmt(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) s_hour24 = !s_hour24;
  sprintf(o->value, "%s", s_hour24 ? "24h" : "12h"); return e == ODROID_DIALOG_ENTER; }
static bool cb_dnd(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) s_dnd = !s_dnd;
  sprintf(o->value, "%s", s_dnd ? curr_lang->s_Clock_On : curr_lang->s_Clock_Off);
  return e == ODROID_DIALOG_ENTER; }
static bool cb_anim(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_anim = (s_anim == 0) ? ANIM_COUNT-1 : s_anim-1;
    if (e == ODROID_DIALOG_NEXT) s_anim = (s_anim+1) % ANIM_COUNT;
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) { if (s_anim == ANIM_GIF) clock_gif_load(); else clock_gif_free(); }
    const char *lv = (s_anim == 0) ? curr_lang->s_Clock_Anim_0
                   : (s_anim == 1) ? curr_lang->s_Clock_Anim_1 : curr_lang->s_Clock_Anim_2;
    snprintf(o->value, sizeof v_anim, "%s", lv);
    return e == ODROID_DIALOG_ENTER;
}
static bool cb_vol(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{   /* edits the SYSTEM volume — same 0..9 scale and curve as games/launcher */
    (void)r;
    int lv = odroid_audio_volume_get();
    if (e == ODROID_DIALOG_PREV && lv > 0) odroid_audio_volume_set(--lv);
    if (e == ODROID_DIALOG_NEXT && lv < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(++lv);
    sprintf(o->value, "%d", lv); return e == ODROID_DIALOG_ENTER; }
static bool cb_enter(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; o->value[0] = 0; return e == ODROID_DIALOG_ENTER; }

static void clock_menu_repaint(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t bg = TH()->scr;
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
    draw_topbar(MODE_CLOCK);
    render_clock(HAL_GetTick(), false);
}

/* returns true when the user picked "Exit" — the caller leaves the app */
static bool clock_settings_menu(void)
{
    odroid_dialog_choice_t opts[] = {
        {0, curr_lang->s_Clock_Anim,   v_anim,   1, cb_anim},
        {1, curr_lang->s_Clock_Format, v_fmt,    1, cb_fmt},
        {2, curr_lang->s_Clock_DND,    v_dnd,    1, cb_dnd},
        {3, curr_lang->s_Clock_Volume, v_vol,    1, cb_vol},
        {4, curr_lang->s_Clock_Alarms, v_alarms, 1, cb_enter},
        {5, curr_lang->s_Clock_Exit,   v_exit,   1, cb_enter},
        ODROID_DIALOG_CHOICE_LAST
    };
    cb_anim(&opts[0], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_fmt(&opts[1], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_dnd(&opts[2], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_vol(&opts[3], ODROID_DIALOG_FOCUS_GAINED, 0);
    v_alarms[0] = 0; v_exit[0] = 0;

    int sel = odroid_overlay_dialog(curr_lang->s_Clock, opts, 0, &clock_menu_repaint, 0);
    if (sel == 4) clock_alarm_setup();
    clock_config_save();
    return sel == 5;
}

/* ---- alarm tone (synthesised, no files) -------------------------------
 *
 * The SAI DMA is already running from boot (circular, double-buffered), so the
 * clock can beep without becoming a separate audio app: start the transmit,
 * fill the active half with a gated square wave each loop, stop on dismiss.
 * The DMA buffer lives in its own .audio section, so this never touches an
 * emulator's RAM. Tones are generated in code — no sound files, no SD needed;
 * user-supplied WAV/MP3 from SD can layer on later. */

#define TONE_HZ   880
static bool     s_tone_on = false;
static uint32_t s_tone_phase = 0;
static uint32_t s_tone_dma_mark = 0;   /* dma_counter of the half we last filled */

static void tone_feed(uint32_t now, bool ringing)
{
    if (!ringing) {
        if (s_tone_on) { audio_stop_playing(); s_tone_on = false; }
        return;
    }
    if (!s_tone_on) { audio_start_playing(AUDIO_BUFFER_LENGTH); s_tone_on = true;
                      s_tone_phase = 0; s_tone_dma_mark = dma_counter - 1; }

    /* Fill each freed half exactly once — the SAI ISR bumps dma_counter per
     * half. The ring loop runs ~8ms vs the 22.4ms half period; refilling the
     * same half every pass advanced the phase 1077 samples per rewrite and put
     * a phase jump at almost every half boundary (audible buzz). */
    if (dma_counter == s_tone_dma_mark) return;
    s_tone_dma_mark = dma_counter;

    int16_t *buf = audio_get_active_buffer();
    int len = audio_get_buffer_length();
    /* alarm loudness follows the SYSTEM volume (identical scale/curve to the
     * rest of the firmware): half-scale square scaled by volume_tbl */
    int amp = (16000 * volume_tbl[odroid_audio_volume_get()]) >> 8;
    bool on = ((now / 250) % 2) == 0;                       /* 250 ms beep / 250 ms gap */
    int period = AUDIO_SAMPLE_RATE / TONE_HZ, half = period / 2;
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (on && amp) {
            s = (s_tone_phase < (uint32_t)half) ? (int16_t)amp : (int16_t)-amp;
            if (++s_tone_phase >= (uint32_t)period) s_tone_phase = 0;
        }
        buf[i] = s;
    }
}

/* ---- main loop -------------------------------------------------------- */

void rg_clock_show(void)
{
    clock_mode_t mode = MODE_CLOCK;
    odroid_gamepad_state_t k, prev = {0};
    uint32_t alarm_ring_until = 0;
    bool dirty = true;

    clock_config_load();
    s_snooze_tick = 0;
    if (s_anim == ANIM_GIF) clock_gif_load();   /* decode /clock/bg.gif once */
    odroid_input_read_gamepad(&prev);   /* swallow the opening button */

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();
        int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();

        /* Alarm first (clock time). Ring = digit pulse + beep for 20s or a
         * NEW key press: A = SNOOZE (ring again in 5 min), anything else =
         * stop — a key already held when it fires must not swallow it, and
         * the press is CONSUMED so it can't fall through and reset a runner,
         * switch mode or open the menu. */
        if (alarm_should_fire(hh, mm)) alarm_ring_until = now + 20000;
        if (s_snooze_tick && now >= s_snooze_tick) {   /* snooze expired */
            s_snooze_tick = 0;
            alarm_ring_until = now + 20000;
        }
        bool ringing = alarm_ring_until > now;
        if (ringing && (k.bitmask & ~prev.bitmask)) {
            alarm_ring_until = 0;
            tone_feed(now, false);
            if (pressed(&k, &prev, ODROID_INPUT_A))
                s_snooze_tick = now + SNOOZE_MS;       /* A = snooze */
            else
                s_snooze_tick = 0;                     /* anything else = off */
            prev = k; dirty = true;
            HAL_Delay(40);
            continue;
        }
        tone_feed(now, ringing);   /* synthesised beep while the alarm rings */

        /* Exit = POWER, or the "Exit" entry in the PAUSE menu — identical in
         * every mode. Face buttons never exit (A/B belong to the runners). */
        if (k.values[ODROID_INPUT_POWER]) break;

        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { mode = (mode == 0) ? MODE_COUNT-1 : mode-1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { mode = (mode+1) % MODE_COUNT; dirty = true; }

        /* PAUSE/SET opens the settings menu in any mode — background, format,
         * DND, alarm volume, alarms, exit. The dialog blocks this loop, so on
         * return ring anything whose minute passed while it was open (the
         * current minute is caught by the regular check above next pass). */
        if (pressed(&k, &prev, ODROID_INPUT_VOLUME)) {
            int pre_mod = hh * 60 + mm;
            bool exit_req = clock_settings_menu();
            if (alarm_fired_in_window(pre_mod, GW_GetCurrentHour() * 60 + GW_GetCurrentMinute()))
                alarm_ring_until = HAL_GetTick() + 20000;
            if (exit_req) break;
            dirty = true;
        }

        /* inputs + state ticks — ticking here (not in render) so a start/stop
         * takes effect immediately even though rendering is signature-gated */
        switch (mode) {
        case MODE_POMODORO:  input_pomodoro(&k, &prev, now);  update_pomodoro(now); break;
        case MODE_TIMER:     input_timer(&k, &prev, now);     update_timer(now);    break;
        case MODE_STOPWATCH: input_stopwatch(&k, &prev, now); tick_countup(&s_watch, now); break;
        default: break;
        }

        /* Only repaint when the visible frame actually changes — saves battery.
         * The signature captures everything on screen for the current mode. */
        static uint32_t last_sig = 0xFFFFFFFF;
        uint32_t sig;
        if (mode == MODE_CLOCK) {
            sig = (1u<<30) | (hh<<20) | (mm<<12) | ((GW_GetCurrentSubSeconds() <= 127)<<11)
                | (s_hour24<<6) | (s_dnd<<5) | (ringing<<4);
            if (s_anim == ANIM_GIF) sig ^= (now / 80);    /* repaint at the GIF rate */
            else if (s_anim > 0)    sig ^= (now / 320);   /* ambient ~3fps */
        }
        else if (mode == MODE_STOPWATCH)
            sig = (2u<<30) | ((s_watch.elapsed_ms / 1000) << 3) | ((uint32_t)s_watch.state << 1)
                | (s_watch.state == RUN_RUNNING ? ((now / 500) & 1) : 0);
        else {
            runner_t *r = (mode == MODE_TIMER) ? &s_timer : &s_pomo;
            sig = ((uint32_t)mode<<28) | ((r->remaining_ms/500)<<4) | (s_pomo_on_break<<3)
                | (uint32_t)r->state;
        }
        /* Full-screen flash is ONLY for the timer/pomodoro end (solid background).
         * The clock alarm must NOT full-screen flash — that would wipe/hide a GIF
         * or ambient background; instead render_clock pulses the time colour. */
        bool flash = s_flash_until > now && ((now/150) & 1);
        if (flash) sig ^= 0x55555555;
        if (ringing) sig ^= (now / 200);   /* repaint for the alarm digit pulse */

        if (dirty || sig != last_sig) {
            last_sig = sig; dirty = false;
            uint16_t *fb = lcd_get_active_buffer();
            uint16_t bg = flash ? TH()->ink : TH()->scr;
            for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
            switch (mode) {
            case MODE_CLOCK:     render_clock(now, ringing); break;
            case MODE_POMODORO:  render_pomodoro(now);  break;
            case MODE_TIMER:     render_timer(now);     break;
            case MODE_STOPWATCH: render_stopwatch(now); break;
            default: break;
            }
            draw_topbar(mode);   /* over the background layers */
            draw_hintbar(ringing ? HINT_RINGING
                : mode == MODE_CLOCK     ? HINT_CLOCK
                : mode == MODE_POMODORO  ? (s_pomo.state == RUN_RUNNING ? HINT_RUN : HINT_TIMER_STOP)
                : mode == MODE_TIMER     ? (s_timer.state == RUN_RUNNING ? HINT_RUN : HINT_TIMER_STOP)
                : HINT_RUN);
            /* the digit pulse only exists on the clock face — give the other
             * modes a visible (and vol=0-proof) ring signal too */
            if (ringing && mode != MODE_CLOCK && ((now / 200) & 1))
                draw_centered_i18n(32, curr_lang->s_Clock_Ringing, TH()->alarm);
            lcd_swap();
            lcd_sleep_while_swap_pending();
        }

        prev = k;
        /* Ringing feeds audio, so keep the buffer fresh; otherwise idle longer. */
        HAL_Delay(ringing ? 8 : 40);
    }

    tone_feed(0, false);   /* make sure the SAI is stopped on the way out */
    clock_gif_free();      /* release the transient GIF cache */
    clock_config_save();
}
