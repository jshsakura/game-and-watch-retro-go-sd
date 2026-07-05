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

/* ---- themes: colour sets + digit faces (both user-selectable) ----------
 * Every face keeps the LCD ghost treatment; text always uses the one
 * firmware i18n font. */

typedef enum { FACE_SEG7 = 0, FACE_PIXEL, FACE_DOT } digit_face_t;

typedef struct { uint16_t scr, ink, alarm; uint8_t face; } clock_theme_t;

static const clock_theme_t THEMES[] = {
    { C565(0x07,0x0a,0x10), C565(0xee,0xf1,0xee), C565(0x33,0xd3,0xc9), FACE_SEG7  }, /* Midnight */
    { C565(0x0a,0x08,0x06), C565(0xff,0xb6,0x38), C565(0xff,0xd9,0x8a), FACE_PIXEL }, /* Amber    */
    { C565(0x0c,0x13,0x0b), C565(0x8f,0xe3,0x6a), C565(0xc8,0xf5,0x9a), FACE_SEG7  }, /* Green LCD*/
    { C565(0x08,0x08,0x08), C565(0xf2,0xed,0xe0), C565(0xe0,0xa9,0x4f), FACE_DOT   }, /* Ivory    */
    { C565(0x12,0x0a,0x07), C565(0xff,0x7a,0x3c), C565(0xff,0xd0,0xa0), FACE_PIXEL }, /* Ember    */
    { C565(0x04,0x12,0x1a), C565(0x4f,0xd6,0xe6), C565(0xa0,0xf0,0xff), FACE_DOT   }, /* Aqua     */
    { C565(0x0a,0x06,0x14), C565(0xc7,0x7d,0xff), C565(0xff,0x7a,0xc8), FACE_SEG7  }, /* Neon     */
    { C565(0x0d,0x0f,0x12), C565(0xdf,0xe6,0xee), C565(0x8f,0xb4,0xd8), FACE_PIXEL }, /* Slate    */
};
#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

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

/* ---- 5x7 pixel/dot faces (with an all-cells ghost glyph) --------------- */

#define PIX_ALL 10
static const uint8_t DOT5x7[11][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {31,31,31,31,31,31,31},
};

static void draw_pix_digit(int d, int x, int y, int px, uint16_t col, bool dot)
{
    if (d < 0 || d > PIX_ALL) return;
    int inset = dot ? 1 : 0, sz = px - (dot ? 2 : 0);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (DOT5x7[d][r] & (1 << (4 - c)))
                odroid_overlay_draw_fill_rect(x + c*px + inset, y + r*px + inset, sz, sz, col);
}

/* Geometry of the big "HH:MM" block per face, so callers can centre extras. */
#define SEG_W    44
#define SEG_H    92
#define SEG_T    10
#define SEG_GAP  10
#define SEG_Y    64   /* digits sit lower: breathing room under the logo bar */
#define PIX_PX   9
#define PIX_Y    (SEG_Y + 6)

static int big_time_width(digit_face_t face)
{
    if (face == FACE_SEG7)
        return 4*SEG_W + 3*SEG_GAP + (SEG_T + 2*SEG_GAP) + SEG_SLANT;
    return 4*(5*PIX_PX) + 3*PIX_PX + (PIX_PX*3);
}

/* Draw "HH:MM" centred; when colon=false the colon drops to the ghost shade.
 * Every segment is first drawn in a faint "ghost" colour, the lit ones on
 * top — the unlit-segment look of a real LCD alarm clock. blank_lead hides a
 * leading zero the way segment clocks do (12h "9:41", not "09:41"). */
/* Two-colour core: hours and minutes can differ (the alarm edit view blinks
 * one field by dropping it to the ghost shade). */
static void draw_big_time_2c(int hh, int mm, bool colon, bool blank_lead,
                             digit_face_t face, uint16_t col_h, uint16_t col_m,
                             uint16_t ghost)
{
    int x = (GW_LCD_WIDTH - big_time_width(face)) / 2;
    int a = hh/10, b = hh%10, c = mm/10, e = mm%10;
    uint16_t cc = colon ? col_h : ghost;

    if (face == FACE_SEG7) {
        int w = SEG_W, h = SEG_H, t = SEG_T, gap = SEG_GAP, y = SEG_Y;
        draw_seg_digit(8, x, y, w, h, t, ghost);   /* 8 lights all segments */
        if (!(blank_lead && a == 0))
            draw_seg_digit(a, x, y, w, h, t, col_h);
        x += w+gap;
        draw_seg_digit(8, x, y, w, h, t, ghost);
        draw_seg_digit(b, x, y, w, h, t, col_h); x += w+gap;
        odroid_overlay_draw_fill_rect(x+gap, y+h/3, t, t, cc);
        odroid_overlay_draw_fill_rect(x+gap, y+2*h/3, t, t, cc);
        x += t+2*gap;
        draw_seg_digit(8, x, y, w, h, t, ghost);
        draw_seg_digit(c, x, y, w, h, t, col_m); x += w+gap;
        draw_seg_digit(8, x, y, w, h, t, ghost);
        draw_seg_digit(e, x, y, w, h, t, col_m);
    } else {
        bool dot = (face == FACE_DOT);
        int px = PIX_PX, dw = 5*px, y = PIX_Y;
        draw_pix_digit(PIX_ALL, x, y, px, ghost, dot);
        if (!(blank_lead && a == 0))
            draw_pix_digit(a, x, y, px, col_h, dot);
        x += dw+px;
        draw_pix_digit(PIX_ALL, x, y, px, ghost, dot);
        draw_pix_digit(b, x, y, px, col_h, dot); x += dw+px;
        odroid_overlay_draw_fill_rect(x+px, y+2*px, px, px, cc);
        odroid_overlay_draw_fill_rect(x+px, y+4*px, px, px, cc);
        x += px*3;
        draw_pix_digit(PIX_ALL, x, y, px, ghost, dot);
        draw_pix_digit(c, x, y, px, col_m, dot); x += dw+px;
        draw_pix_digit(PIX_ALL, x, y, px, ghost, dot);
        draw_pix_digit(e, x, y, px, col_m, dot);
    }
}

static void draw_big_time(int hh, int mm, bool colon, bool blank_lead,
                          digit_face_t face, uint16_t col, uint16_t ghost)
{
    draw_big_time_2c(hh, mm, colon, blank_lead, face, col, col, ghost);
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
    fill_disc(x + 7, y + 7, 7, col);
    fill_disc(x + 10, y + 5, 6, bg);
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
    /* 48 dots, up to ~20 lit at once, 1-3px, bright at pulse peak — the old
     * 26x1px version was invisible on the real LCD */
    uint16_t dim = (col >> 1) & 0x7BEF;   /* half-bright ink */
    uint32_t ph = now / 320;
    for (int i = 0; i < 48; i++) {
        int x = (i*67 + 13) % (GW_LCD_WIDTH - 3);
        int y = 40 + (i*113 + 9) % (GW_LCD_HEIGHT - 40 - 32);
        int p = (ph + i*5) % 11;
        if (p < 4) {
            int peak = (p == 1 || p == 2);
            int sz = peak ? ((i & 3) == 0 ? 3 : 2) : 1;
            odroid_overlay_draw_fill_rect(x, y, sz, sz, peak ? col : dim);
        }
    }
}

/* ---- i18n text: centred line (REAL glyph metrics via rg_i18n) ---------- */

static void draw_centered_i18n(int y, const char *text, uint16_t col)
{
    int x = (GW_LCD_WIDTH - i18n_get_text_width(text)) / 2;
    if (x < 0) x = 0;
    i18n_draw_text_line(x, y, GW_LCD_WIDTH - x, text, col, CLOCK_BLACK, 1);
}

/* ---- config + alarms (/clock.cfg) ------------------------------------- */

#define CLOCK_CFG_PATH  "/clock.cfg"
#define MAX_ALARMS      8

typedef struct { uint8_t hour, min, enabled; } alarm_t;

static int      s_theme;
static int      s_face_override = -1;   /* -1 = the theme's face, else FACE_* */
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
#define ANIM_COUNT 4
#define ANIM_SCENE 2   /* built-in pixel skyline (the mockup's bundled art) */
#define ANIM_GIF   3

static void clock_config_load(void)
{
    s_theme = 0; s_face_override = -1;
    s_hour24 = false; s_dnd = false; s_anim = 0;
    s_alarm_count = 0;
    FILE *f = fopen(CLOCK_CFG_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int v, en = 1;
        if (sscanf(line, "theme=%d", &v) == 1) { if (v >= 0 && v < THEME_COUNT) s_theme = v; }
        else if (sscanf(line, "face=%d", &v) == 1) { if (v >= -1 && v <= FACE_DOT) s_face_override = v; }
        else if (sscanf(line, "hour24=%d", &v) == 1) s_hour24 = v != 0;
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
    fprintf(f, "theme=%d\n", s_theme);
    fprintf(f, "face=%d\n", s_face_override);
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

static const clock_theme_t *TH(void) { return &THEMES[s_theme]; }

static digit_face_t cur_face(void)
{ return (s_face_override >= 0) ? (digit_face_t)s_face_override : (digit_face_t)TH()->face; }

#define STATUS_Y 178   /* the one shared status-line row under the digits */

/* blend a toward b by t/16 per RGB565 channel */
static uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    int ar = (a>>11)&31, ag = (a>>5)&63, ab = a&31;
    int br = (b>>11)&31, bg = (b>>5)&63, bb = b&31;
    return (uint16_t)(((ar+(br-ar)*t/16)<<11) | ((ag+(bg-ag)*t/16)<<5) | (ab+(bb-ab)*t/16));
}

/* Built-in pixel scene: a night skyline with slowly twinkling windows and a
 * few stars, tinted from the active theme — the bundled background the HTML
 * mockups always had. Pure code, no SD assets; repaints ~1.5 fps. */
static void draw_scene(uint32_t now, const clock_theme_t *t)
{
    uint16_t sil  = mix565(t->scr, t->ink, 3);
    uint16_t sil2 = mix565(t->scr, t->ink, 2);
    uint16_t win  = mix565(t->scr, t->alarm, 10);
    uint16_t wdim = mix565(t->scr, t->alarm, 5);
    uint32_t ph = now / 640;

    /* far layer: low rooftops */
    for (int x = 0, i = 0; x < GW_LCD_WIDTH; i++) {
        int w = 26 + ((i * 41) % 19), hgt = 10 + ((i * 29) % 14);
        odroid_overlay_draw_fill_rect(x, GW_LCD_HEIGHT - hgt, w, hgt, sil2);
        x += w;
    }
    /* near layer: taller buildings with windows */
    for (int x = 2, i = 0; x < GW_LCD_WIDTH - 12; i++) {
        int w = 20 + ((i * 37) % 21), hgt = 22 + ((i * 53) % 32);
        int top = GW_LCD_HEIGHT - hgt;
        if (x + w > GW_LCD_WIDTH) w = GW_LCD_WIDTH - x;
        odroid_overlay_draw_fill_rect(x, top, w, hgt, sil);
        for (int wy = top + 4; wy + 4 < GW_LCD_HEIGHT; wy += 8)
            for (int wx = x + 3; wx + 3 < x + w - 2; wx += 7) {
                uint32_t hsh = (uint32_t)(wx * 31 + wy * 17 + i * 7);
                if ((hsh % 9) < 3)   /* ~1/3 of window slots exist */
                    odroid_overlay_draw_fill_rect(wx, wy, 3, 4,
                        ((hsh + ph) % 13) < 10 ? win : wdim);   /* slow twinkle */
            }
        x += w + 3;
    }
    /* a few stars above the skyline */
    for (int k = 0; k < 20; k++) {
        int sx = (k * 71 + 9) % GW_LCD_WIDTH;
        int sy = 42 + (k * 97 + 5) % 110;
        if (((now / 640) + k * 3) % 7 < 2)
            odroid_overlay_draw_fill_rect(sx, sy, 1, 1, mix565(t->scr, t->ink, 8));
    }
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
/* Everything in the bar is vertically centred on the REAL G&W logo (35x30
 * at y=8, centre row 23): battery 10px -> y=18, moon 14px -> y=16, title
 * 12px -> y=12 with the pager dots under it. No bell up here — the
 * next-alarm line already carries it.
 *
 * with_title: the mode name + pager dots suit navigation, not a clean clock
 * face — on MODE_CLOCK they appear for a moment after entry/mode switch and
 * then fade, leaving logo/battery only. Other modes keep them. */
static void draw_topbar(clock_mode_t mode, bool with_title)
{
    const clock_theme_t *t = TH();
    odroid_overlay_draw_logo(9, 8, RG_LOGO_GNW, t->ink);
    odroid_overlay_draw_battery(odroid_input_read_battery(), GW_LCD_WIDTH - 26, 18);
    if (s_dnd) draw_moon(GW_LCD_WIDTH - 48, 16, t->ink, t->scr);
    /* pager dots stay ALWAYS (tiny, and good mode context);
     * only the title text fades on the clock face */
    int dx = (GW_LCD_WIDTH - (MODE_COUNT*8 - 4)) / 2;
    for (int i = 0; i < MODE_COUNT; i++, dx += 8)
        odroid_overlay_draw_fill_rect(dx, 28, 4, 4,
            i == (int)mode ? t->alarm : mix565(t->scr, t->ink, 4));
    if (!with_title) return;

    char line[48]; snprintf(line, sizeof line, "\xe2\x97\x80 %s \xe2\x96\xb6", mode_title(mode));
    draw_centered_i18n(12, line, t->alarm);
}

#define TITLE_SHOW_MS 2500
static uint32_t s_title_until = 0;   /* clock-face mode-title fade deadline */

/* Bottom hint: ALWAYS visible, in the firmware's default 8px font (crisp,
 * matches the rest of the OS chrome — the 12px serif looked broken here),
 * sitting on a rounded pill so it reads as one quiet control strip.
 *
 * NOTE the device blitters do NOT clip: a rect or text cell that crosses the
 * right edge wraps into the next row's left side (the "crumbs" beside the
 * old hint bar). Everything here is therefore clamped to the screen. */
static void draw_round_panel(int x, int y, int w, int h, int r, uint16_t col)
{
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? r - 1 - j : (j >= h - r ? j - (h - r - 1) : -1);
        int inset = 0;
        if (dy >= 0) {   /* largest k with k^2 + dy^2 <= r^2 = quarter circle */
            inset = r;
            for (int k = r; k >= 0; k--)
                if (k*k + dy*dy <= r*r) { inset = r - k; break; }
        }
        int rx = x + inset, rw = w - 2*inset;
        if (rx < 0) { rw += rx; rx = 0; }
        if (rx + rw > GW_LCD_WIDTH) rw = GW_LCD_WIDTH - rx;
        if (rw > 0) odroid_overlay_draw_fill_rect(rx, y + j, rw, 1, col);
    }
}

static void draw_hintbar(const char *hint)
{
    const clock_theme_t *t = TH();
    uint16_t panel = mix565(t->scr, t->ink, 2);
    uint16_t txt   = mix565(t->scr, t->ink, 9);
    int w = i18n_get_text_width(hint) + 2;
    int maxw = GW_LCD_WIDTH - 32;          /* pill padding + side margins */
    if (w > maxw) w = maxw;
    int x = (GW_LCD_WIDTH - w) / 2, y = GW_LCD_HEIGHT - 24;
    draw_round_panel(x - 12, y - 3, w + 24, 18, 8, panel);
    /* i18n text, transparent, clipped to w by the renderer itself */
    i18n_draw_text_line(x, y, w, hint, txt, panel, 1);
}

/* Hint legends are fixed ASCII (button names are Latin on the shell anyway)
 * so the 8px font can render them in every language. */


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
    else if (s_anim == ANIM_SCENE)
        draw_scene(now, t);
    else if (s_anim == 1)
        draw_ambient(now, t->ink);

    /* date + weekday, centred UNDER the top bar (was colliding with it) */
    char date[48];
    snprintf(date, sizeof date, "%02d/%02d %s", GW_GetCurrentMonth(), GW_GetCurrentDay(), weekday_str());
    draw_centered_i18n(42, date, t->ink);

    /* big time. When the alarm is ringing the digits PULSE between ink and
     * the accent (~2.5 Hz): a clear alarm signal that sits on top of the
     * GIF/ambient background instead of a full-screen flash covering it. */
    int dh = hh;
    if (!s_hour24) { dh = hh % 12; if (dh == 0) dh = 12; }
    uint16_t timecol = t->ink;
    if (alarm_firing && ((now / 200) & 1)) timecol = t->alarm;
    digit_face_t face = cur_face();
    draw_big_time(dh, mm, colon, !s_hour24, face, timecol, mix565(t->scr, t->ink, 2));

    /* AM/PM tucked to the RIGHT of the digits at their baseline (G&W style),
     * not a whole centred line of its own */
    if (!s_hour24) {
        int x = (GW_LCD_WIDTH + big_time_width(face)) / 2 + 6;
        int yb = (face == FACE_SEG7) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
        i18n_draw_text_line(x, yb, GW_LCD_WIDTH - x,
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
            int lx = (GW_LCD_WIDTH - i18n_get_text_width(line)) / 2;
            draw_bell(lx - 16, STATUS_Y + 1, alcol);
            draw_centered_i18n(STATUS_Y, line, alcol);
        }
    }
}

static void render_mmss(uint32_t ms, uint16_t col, bool colon)
{
    uint32_t total = ms / 1000; int m = (total / 60) % 100, s = total % 60;
    draw_big_time(m, s, colon, false, cur_face(), col, mix565(TH()->scr, TH()->ink, 2));
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
    (void)now;
    const clock_theme_t *t = TH();
    render_mmss(s_watch.elapsed_ms, t->ink, true);
    /* centiseconds in the AM/PM slot: MM:SS big + .cc small = 6-digit read */
    char cs[8]; snprintf(cs, sizeof cs, ".%02u", (unsigned)((s_watch.elapsed_ms / 10) % 100));
    digit_face_t face = cur_face();
    int x = (GW_LCD_WIDTH + big_time_width(face)) / 2 + 6;
    int yb = (face == FACE_SEG7) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
    i18n_draw_text_line(x, yb, GW_LCD_WIDTH - x, cs, t->alarm, CLOCK_BLACK, 1);
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

/* Full-screen alarm edit: the SAME big-digit face as the clock (clone view),
 * with the active field blinking to the ghost shade — set the time the way a
 * real alarm clock does, not through a text row. */
static void render_alarm_edit(const alarm_t *a, int field, bool blink_off)
{
    const clock_theme_t *t = TH();
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = t->scr;

    char title[64];
    snprintf(title, sizeof title, "%s", curr_lang->s_Clock_Alarms);
    int tw = i18n_get_text_width(title);
    draw_bell((GW_LCD_WIDTH - tw) / 2 - 16, 12, t->alarm);
    draw_centered_i18n(12, title, t->alarm);

    int dh = a->hour;
    if (!s_hour24) { dh = a->hour % 12; if (dh == 0) dh = 12; }
    digit_face_t face = cur_face();
    uint16_t ghost = mix565(t->scr, t->ink, 2);
    uint16_t ch = (field == 0 && blink_off) ? ghost : t->ink;
    uint16_t cm = (field == 1 && blink_off) ? ghost : t->ink;
    draw_big_time_2c(dh, a->min, true, !s_hour24, face, ch, cm, ghost);

    if (!s_hour24) {
        int x = (GW_LCD_WIDTH + big_time_width(face)) / 2 + 6;
        int yb = (face == FACE_SEG7) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
        i18n_draw_text_line(x, yb, GW_LCD_WIDTH - x,
                            a->hour < 12 ? curr_lang->s_AM : curr_lang->s_PM, t->ink, CLOCK_BLACK, 1);
    }
    draw_hintbar(curr_lang->s_Clock_Hint_Edit);
}

/* Returns true = confirmed (A), false = cancelled (B). Edits *a in place;
 * the caller keeps a backup for the cancel path. */
static bool alarm_edit_view(alarm_t *a)
{
    int field = 0; bool dirty = true;
    odroid_gamepad_state_t k, prev = {0};
    odroid_input_read_gamepad(&prev);
    uint32_t last_phase = ~0u;
    bool result = true;

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();

        if (pressed(&k, &prev, ODROID_INPUT_A)) { result = true; break; }
        if (pressed(&k, &prev, ODROID_INPUT_B)) { result = false; break; }
        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { field = 0; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { field = 1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_UP))   { if (field == 0) a->hour = (a->hour+1)%24; else a->min = (a->min+1)%60; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { if (field == 0) a->hour = (a->hour==0)?23:a->hour-1; else a->min = (a->min==0)?59:a->min-1; dirty = true; }

        uint32_t phase = now / 500;   /* field blink */
        if (dirty || phase != last_phase) {
            dirty = false; last_phase = phase;
            render_alarm_edit(a, field, (phase & 1) != 0);
            lcd_swap(); lcd_sleep_while_swap_pending();
        }
        prev = k;
        HAL_Delay(40);
    }
    /* swallow the closing press before returning to the list */
    do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); } while (k.bitmask);
    return result;
}

/* A proper popup: clean solid base + bordered panel, repainted whole every
 * frame. (The old version re-scrimmed whatever was already on the swap
 * buffer, so each repaint stacked another darkening layer + stale rows.) */
static void render_alarm_setup(int sel)
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
            snprintf(line, sizeof line, "%s        %s", ts, tag);
        } else if (i == s_alarm_count) snprintf(line, sizeof line, "+ %s", curr_lang->s_Clock_Add_Alarm);
        else snprintf(line, sizeof line, "%s", curr_lang->s_Clock_Done);
        draw_centered_i18n(y, line, col);
    }
    draw_hintbar(curr_lang->s_Clock_Hint_Editor);
}

static void alarm_delete_at(int sel)
{
    for (int i = sel; i < s_alarm_count - 1; i++) s_alarms[i] = s_alarms[i+1];
    s_alarm_count--;
}

static void clock_alarm_setup(void)
{
    int sel = 0;
    bool dirty = true;
    odroid_gamepad_state_t k, prev = {0};
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        int rows = s_alarm_count + 2;

        if (pressed(&k, &prev, ODROID_INPUT_B)) break;
        if (pressed(&k, &prev, ODROID_INPUT_UP))   { sel = (sel == 0) ? rows-1 : sel-1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { sel = (sel+1) % rows; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_A)) {
            if (sel < s_alarm_count) {
                /* edit in the full-screen clone view; B there restores */
                alarm_t backup = s_alarms[sel];
                if (!alarm_edit_view(&s_alarms[sel]))
                    s_alarms[sel] = backup;
                odroid_input_read_gamepad(&prev);
            } else if (sel == s_alarm_count) {
                if (s_alarm_count < MAX_ALARMS) {
                    s_alarms[s_alarm_count] = (alarm_t){ 7, 0, 1 };
                    sel = s_alarm_count; s_alarm_count++;
                    /* cancel on a fresh add = the alarm never existed */
                    if (!alarm_edit_view(&s_alarms[sel])) {
                        alarm_delete_at(sel);
                        if (sel >= s_alarm_count && sel > 0) sel--;
                    }
                    odroid_input_read_gamepad(&prev);
                }
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

        if (dirty) { dirty = false; render_alarm_setup(sel);
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

static const char *const THEME_LABEL[THEME_COUNT] =
    { "Midnight", "Amber", "Green LCD", "Ivory", "Ember", "Aqua", "Neon", "Slate" };
static const char *const FACE_NAME[3] = { "7-seg", "Pixel", "Dot" };
static char v_theme[24], v_face[20], v_fmt[8], v_dnd[12], v_anim[44],
            v_vol[ODROID_AUDIO_VOLUME_MAX + 2], v_alarms[4], v_exit[4];

static bool cb_theme(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_theme = (s_theme == 0) ? THEME_COUNT-1 : s_theme-1;
    if (e == ODROID_DIALOG_NEXT) s_theme = (s_theme+1) % THEME_COUNT;
    sprintf(o->value, "%s", THEME_LABEL[s_theme]);
    return e == ODROID_DIALOG_ENTER;
}
static bool cb_face(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_face_override = (s_face_override <= -1) ? FACE_DOT : s_face_override-1;
    if (e == ODROID_DIALOG_NEXT) s_face_override = (s_face_override >= FACE_DOT) ? -1 : s_face_override+1;
    if (s_face_override < 0) sprintf(o->value, "%s", curr_lang->s_Clock_Auto);
    else sprintf(o->value, "%s", FACE_NAME[s_face_override]);
    return e == ODROID_DIALOG_ENTER;
}

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
                   : (s_anim == 1) ? curr_lang->s_Clock_Anim_1
                   : (s_anim == ANIM_SCENE) ? curr_lang->s_Clock_Anim_2
                   : curr_lang->s_Clock_Anim_3;
    const char *why = "";
    if (s_anim == ANIM_GIF && !clock_gif_ready())
        why = (clock_gif_status() == CLOCK_GIF_NO_RAM)  ? " (no RAM)"
            : (clock_gif_status() == CLOCK_GIF_BAD_DIMS) ? " (too big)"
            : " (no file)";
    snprintf(o->value, sizeof v_anim, "%s%s", lv, why);
    return e == ODROID_DIALOG_ENTER;
}
static bool cb_vol(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{   /* edits the SYSTEM volume — same 0..9 scale, curve AND bar-gauge look
     * as the common volume row (odroid_overlay.c volume_update_cb) */
    int lv = odroid_audio_volume_get();
    if (e == ODROID_DIALOG_PREV && lv > 0) odroid_audio_volume_set(--lv);
    if (e == ODROID_DIALOG_NEXT && lv < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(++lv);
    char a = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Fill[0] : curr_lang->s_Full[0];
    char b = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Full[0] : curr_lang->s_Fill[0];
    for (int i = 0; i <= ODROID_AUDIO_VOLUME_MAX; i++) o->value[i] = (i <= lv) ? a : b;
    o->value[ODROID_AUDIO_VOLUME_MAX + 1] = 0;
    return e == ODROID_DIALOG_ENTER; }
static bool cb_enter(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; o->value[0] = 0; return e == ODROID_DIALOG_ENTER; }

static void clock_menu_repaint(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t bg = TH()->scr;
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
    draw_topbar(MODE_CLOCK, true);
    render_clock(HAL_GetTick(), false);
}

/* returns true when the user picked "Exit" — the caller leaves the app */
static bool clock_settings_menu(void)
{
    odroid_dialog_choice_t opts[] = {
        {0, curr_lang->s_Clock_Theme,  v_theme,  1, cb_theme},
        {1, curr_lang->s_Clock_Face,   v_face,   1, cb_face},
        {2, curr_lang->s_Clock_Anim,   v_anim,   1, cb_anim},
        {3, curr_lang->s_Clock_Format, v_fmt,    1, cb_fmt},
        {4, curr_lang->s_Clock_DND,    v_dnd,    1, cb_dnd},
        {5, curr_lang->s_Clock_Volume, v_vol,    1, cb_vol},
        {6, curr_lang->s_Clock_Alarms, v_alarms, 1, cb_enter},
        {7, curr_lang->s_Clock_Exit,   v_exit,   1, cb_enter},
        ODROID_DIALOG_CHOICE_LAST
    };
    cb_theme(&opts[0], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_face(&opts[1], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_anim(&opts[2], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_fmt(&opts[3], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_dnd(&opts[4], ODROID_DIALOG_FOCUS_GAINED, 0);
    cb_vol(&opts[5], ODROID_DIALOG_FOCUS_GAINED, 0);
    v_alarms[0] = 0; v_exit[0] = 0;

    int sel = odroid_overlay_dialog(curr_lang->s_Clock, opts, 0, &clock_menu_repaint, 0);
    if (sel == 6) clock_alarm_setup();
    clock_config_save();
    return sel == 7;
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
    s_title_until = HAL_GetTick() + TITLE_SHOW_MS;

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

        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { mode = (mode == 0) ? MODE_COUNT-1 : mode-1; dirty = true; s_title_until = now + TITLE_SHOW_MS; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { mode = (mode+1) % MODE_COUNT; dirty = true; s_title_until = now + TITLE_SHOW_MS; }

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
                | (s_theme<<7) | (s_hour24<<6) | (s_dnd<<5) | (ringing<<4);
            if (s_anim == ANIM_GIF)        sig ^= (now / 80);   /* GIF rate */
            else if (s_anim == ANIM_SCENE) sig ^= (now / 640);  /* window twinkle */
            else if (s_anim > 0)           sig ^= (now / 320);  /* ambient ~3fps */
        }
        else if (mode == MODE_STOPWATCH)
            sig = (2u<<30) | ((s_watch.elapsed_ms / 10) << 2) | (uint32_t)s_watch.state;
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
        if (mode == MODE_CLOCK && s_title_until > now) sig ^= (1u<<26);   /* title fade */

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
            bool with_title = (mode != MODE_CLOCK) || now < s_title_until;
            draw_topbar(mode, with_title);   /* over the background layers */
            draw_hintbar(ringing ? curr_lang->s_Clock_Hint_Ring
                : mode == MODE_CLOCK     ? curr_lang->s_Clock_Hint_Clock
                : mode == MODE_POMODORO  ? (s_pomo.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                        : curr_lang->s_Clock_Hint_TimerStop)
                : mode == MODE_TIMER     ? (s_timer.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                         : curr_lang->s_Clock_Hint_TimerStop)
                : curr_lang->s_Clock_Hint_Run);
            /* the digit pulse only exists on the clock face — give the other
             * modes a visible (and vol=0-proof) ring signal too */
            if (ringing && mode != MODE_CLOCK && ((now / 200) & 1))
                draw_centered_i18n(42, curr_lang->s_Clock_Ringing, TH()->alarm);
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
