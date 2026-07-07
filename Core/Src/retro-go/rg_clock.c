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
 * A start/pause, B reset, PAUSE = settings (incl. Exit); POWER sleeps and
 * resumes back INTO the clock (it does not quit); while
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
#include <strings.h>   /* strcasecmp — file-picker extension match (SD builds) */

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
#include "common.h"   /* volume_tbl — shared 0..9 loudness curve (the alarm uses its own s_alarm_volume index, not the system volume) */
#include "rg_storage.h" /* rg_storage_mkdir — ensure /clock exists on FatFs and LittleFS alike */
#include "bq24072.h"  /* bq24072_get_state — charging exception for the idle backlight */
#include "rg_clock.h"
#include "rg_alarm.h"   /* resident next-alarm cache + shared tone presets (all-state alarm) */
#include "rg_clock_gif.h"
#include "rg_clock_album.h"
#include "rg_clock_alarm_mp3.h"

/* forward-declared to keep the heavy rg_emulators.h out of this TU (and the host
 * clock preview harness); rg_clock already pulls in gui.h for the tab accessors. */
extern void rg_emulators_reset_all_lists(void);

/* User-media backgrounds/sounds (photo album, GIF background, MP3/WAV alarm) all
 * need a writable place to RECEIVE files, which a card-less unit has no way to
 * do — so they are compiled out entirely on SD_CARD=0 (their .c files are also
 * dropped from the Makefile). The background picker then offers only Off/Scene
 * (pixel scenes are procedural — kept on both builds), a saved GIF/photo anim is
 * clamped back to Off, and the alarm is beep-only. SD_CARD is always -D'd by the
 * firmware build; an undefined value (host logic tests) reads as the flash case. */
#if SD_CARD == 1
#define CLOCK_SD_MEDIA 1
#else
#define CLOCK_SD_MEDIA 0
#endif

#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define CLOCK_BLACK 0x0000

/* ---- themes: colour sets + digit faces (both user-selectable) ----------
 * Every face keeps the LCD ghost treatment; text always uses the one
 * firmware i18n font. */

/* Digit faces. The first three are the originals — their integer values are
 * LOCKED because they are persisted verbatim in /clock/clock.cfg (face=N); new
 * faces are only ever APPENDED after them so an old config still selects the
 * same look. FACE_COUNT is the sentinel. Faces split into two geometry
 * families: the vector 7-seg block (SEG_W..) and the 5x7 pixel grid (PIX_PX);
 * face_is_pixel() classifies a face so the shared layout math (block width,
 * AM/PM baseline, stopwatch) works for every face without per-face branches. */
typedef enum {
    FACE_SEG7 = 0, FACE_PIXEL, FACE_DOT,     /* originals — values locked */
    FACE_THIN, FACE_OUTLINE, FACE_FACET, FACE_FLIP, FACE_LED, FACE_LCD,
    FACE_COUNT
} digit_face_t;
#define FACE_LAST (FACE_COUNT - 1)

/* pixel-grid family (square pixels / inset dots / round LEDs); everything else
 * is a 7-seg-geometry face drawn in the SEG_W×SEG_H block. */
static inline bool face_is_pixel(digit_face_t f)
{ return f == FACE_PIXEL || f == FACE_DOT || f == FACE_LED; }

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
    /* ---- new: 6 more tasteful palettes (appended — indices are persisted) */
    { C565(0x06,0x12,0x0e), C565(0xb8,0xf2,0xd8), C565(0xff,0x8a,0x9e), FACE_THIN    }, /* Mint     */
    { C565(0x12,0x08,0x0e), C565(0xff,0xcd,0xe0), C565(0xff,0x5f,0x9c), FACE_DOT     }, /* Sakura   */
    { C565(0x05,0x0c,0x14), C565(0xd7,0xef,0xff), C565(0x5a,0xd8,0xff), FACE_LCD     }, /* Arctic   */
    { C565(0x07,0x0d,0x08), C565(0x9b,0xd9,0x7a), C565(0xe0,0xb8,0x4a), FACE_SEG7    }, /* Forest   */
    { C565(0x00,0x00,0x00), C565(0xff,0xff,0xff), C565(0xff,0x33,0x33), FACE_OUTLINE }, /* OLED     */
    { C565(0x00,0x00,0x00), C565(0xff,0xb0,0x00), C565(0xff,0xe0,0x66), FACE_LED     }, /* Term     */
};
#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

/* ---- 7-segment vector digit --------------------------------------------
 * Real-LCD styling: every segment is a HEXAGON (ends taper to a point at
 * 45°) and the whole digit leans right by SEG_SLANT px (italic), the way
 * DSEG-style clock faces do — not seven butted rectangles. Drawn as 1px
 * rows so the shear is exact; still just fill_rect calls underneath. */

static const uint8_t SEG7[10] = { 0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F };

/* Legibility over photos: when s_outline > 0, every digit is first painted as a
 * dark 4-way halo (offset s_outline px) in s_outline_col, so the ink reads over
 * ANY background. Zero on solid themes (the ghost-8 does that job there). */
static int      s_outline = 0;
static uint16_t s_outline_col = 0;

static void draw_seg_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9) return;
    if (s_outline) {
        int o = s_outline; uint16_t oc = s_outline_col; s_outline = 0;   /* guard recursion */
        draw_seg_digit(d, x - o, y, w, h, t, oc); draw_seg_digit(d, x + o, y, w, h, t, oc);
        draw_seg_digit(d, x, y - o, w, h, t, oc); draw_seg_digit(d, x, y + o, w, h, t, oc);
        s_outline = o;
    }
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
                odroid_overlay_draw_fill_rect(x + S[s].x0 + inset,
                                              y + yr, S[s].len - 2*inset, 1, col);
            }
        } else {
            for (int r = 0; r < S[s].len; r++) {    /* vertical hexagon */
                int e = r < S[s].len - 1 - r ? r : S[s].len - 1 - r;
                int wid = 2*e + 1; if (wid > t - 1) wid = t - 1;
                int yr = S[s].y0 + r;
                odroid_overlay_draw_fill_rect(x + S[s].x0 + (t - wid)/2,
                                              y + yr, wid, 1, col);
            }
        }
    }
}

/* forward decls: these decorative faces reuse the colour blend + rounded panel
 * helpers that are defined further down (rendering section). */
static uint16_t mix565(uint16_t a, uint16_t b, int t);
static void draw_round_panel(int x, int y, int w, int h, int r, uint16_t col);
static void draw_round_panel_ex(int x, int y, int w, int h, int r, uint16_t col, bool blend);
/* true on a solid theme background (no live wallpaper under the digits) —
 * defined further down with the render state; forward-declared so Flip's
 * card (drawn earlier in the file) can decide opaque-vs-blend at draw time. */
static bool s_ghost_on;

/* ---- upright rectangular 7-seg engine (shared by Thin + Retro-LCD) ------
 * One parameterised drawer covers both upright (non-italic) seg faces: `tt` is
 * the stroke thickness, `hg`/`vg` the corner insets (small gaps so segments do
 * not fuse), `rounded` picks capsule caps (Thin, a fine LCD-watch stroke) vs
 * square ends (Retro-LCD, a fat calculator segment). Same SEG_W×SEG_H block as
 * FACE_SEG7 so all the shared layout math still holds. */
static void seg_bar(int x, int y, int len, int tt, bool vert, bool rounded, uint16_t col)
{
    for (int i = 0; i < len; i++) {
        int cap = 0;
        if (rounded) {
            if (i < tt/2)              cap = tt/2 - i;
            else if (i > len-1 - tt/2) cap = i - (len-1 - tt/2);
        }
        int th = tt - 2*cap; if (th < 1) continue;
        if (vert) odroid_overlay_draw_fill_rect(x + cap, y + i, th, 1, col);
        else      odroid_overlay_draw_fill_rect(x + i, y + cap, 1, th, col);
    }
}

static void draw_rect7(int d, int x, int y, int w, int h, int tt, int hg, int vg,
                       bool rounded, uint16_t col)
{
    if (d < 0 || d > 9) return;
    if (s_outline) {
        int o = s_outline; uint16_t oc = s_outline_col; s_outline = 0;
        draw_rect7(d, x-o, y, w, h, tt, hg, vg, rounded, oc);
        draw_rect7(d, x+o, y, w, h, tt, hg, vg, rounded, oc);
        draw_rect7(d, x, y-o, w, h, tt, hg, vg, rounded, oc);
        draw_rect7(d, x, y+o, w, h, tt, hg, vg, rounded, oc);
        s_outline = o;
    }
    uint8_t m = SEG7[d];
    int ym = y + (h - tt)/2;
    int hx = x + hg, hw = w - 2*hg, xr = x + w - tt;
    int vt = y + tt + vg, vtl = ym - (y + tt) - vg;            /* top half column */
    int vb = ym + tt + vg, vbl = (y + h - tt) - (ym + tt) - vg;/* bottom half column */
    if (m & (1<<0)) seg_bar(hx, y,        hw,  tt, false, rounded, col);  /* A */
    if (m & (1<<6)) seg_bar(hx, ym,       hw,  tt, false, rounded, col);  /* G */
    if (m & (1<<3)) seg_bar(hx, y + h-tt, hw,  tt, false, rounded, col);  /* D */
    if (m & (1<<5)) seg_bar(x,  vt,       vtl, tt, true,  rounded, col);  /* F */
    if (m & (1<<1)) seg_bar(xr, vt,       vtl, tt, true,  rounded, col);  /* B */
    if (m & (1<<4)) seg_bar(x,  vb,       vbl, tt, true,  rounded, col);  /* E */
    if (m & (1<<2)) seg_bar(xr, vb,       vbl, tt, true,  rounded, col);  /* C */
}

/* Thin: half-thickness rounded capsule strokes; the corners inset by the full
 * stroke so the caps meet cleanly (vg=0). */
static void draw_thin_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{ int tt = t/2; if (tt < 3) tt = 3; draw_rect7(d, x, y, w, h, tt, tt, 0, true, col); }

/* Retro-LCD: classic calculator-style square segments — thinner than a solid
 * block (canonical 7-seg stroke is a fraction of the digit width, not the
 * whole cell) with a wider corner gap so adjoining segments read as distinct
 * bars, not a fused blob. The faint "all-8" ghost underlay (see seg_cell) is
 * what actually sells the period-LCD look. */
static void draw_lcd_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{ int tt = t*3/5; if (tt < 4) tt = 4; int g = t/2; if (g < 2) g = 2;
  draw_rect7(d, x, y, w, h, tt, g, g, false, col); }

/* ---- Outline: bold hollow 7-seg ----------------------------------------
 * The base hexagon digit with a darker inner digit punched into it, leaving a
 * ~2px rim — reads as an outlined/hollow numeral. Reuses draw_seg_digit twice
 * (no new geometry). The inner "hole" is a near-black shade of the ink, which
 * blends into the dark theme background (all themes have a dark screen). */
static void draw_outline_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9) return;
    draw_seg_digit(d, x, y, w, h, t, col);        /* rim + s_outline halo */
    int o = s_outline; s_outline = 0;
    int t2 = t - 4;
    if (t2 >= 2) draw_seg_digit(d, x+2, y+2, w-4, h-4, t2, mix565(col, CLOCK_BLACK, 13));
    s_outline = o;
}

/* ---- Facet: low-poly stacked-triangle digit -----------------------------
 * Every 7-seg bar is split by ONE diagonal into two triangles — a lighter
 * tint on one side, a darker shade on the other — so each bar reads as a
 * faceted origami wedge instead of a flat block; the seven wedges stack into
 * a low-poly numeral. Same upright box layout as Thin/Retro-LCD (draw_rect7's
 * S[] geometry), but the fill is a per-row half-space test (pure scanline
 * triangle math) instead of a solid seg_bar — no new heavy primitive, still
 * just fill_rect calls, and every span stays inside the digit's own box so it
 * needs no extra clamping beyond that (same invariant as every other face). */
static void seg_bar_facet(int x, int y, int len, int tt, bool vert, uint16_t hi, uint16_t lo)
{
    for (int r = 0; r < tt; r++) {
        int split = len * (tt - r) / tt;    /* half-space cut: shrinks as r grows */
        if (split > 0) {
            if (vert) odroid_overlay_draw_fill_rect(x + r, y,         1, split, hi);
            else      odroid_overlay_draw_fill_rect(x,     y + r, split,     1, hi);
        }
        if (split < len) {
            int rem = len - split;
            if (vert) odroid_overlay_draw_fill_rect(x + r, y + split, 1, rem, lo);
            else      odroid_overlay_draw_fill_rect(x + split, y + r, rem, 1, lo);
        }
    }
}

static void draw_facet_digit(int d, int x, int y, int w, int h, int t, uint16_t col)
{
    if (d < 0 || d > 9) return;
    if (s_outline) {
        int o = s_outline; uint16_t oc = s_outline_col; s_outline = 0;
        draw_facet_digit(d, x-o, y, w, h, t, oc); draw_facet_digit(d, x+o, y, w, h, t, oc);
        draw_facet_digit(d, x, y-o, w, h, t, oc); draw_facet_digit(d, x, y+o, w, h, t, oc);
        s_outline = o;
    }
    uint8_t m = SEG7[d];
    int tt = t - 2; if (tt < 4) tt = 4;      /* small gap so wedges read as separate facets */
    int ym = y + (h - tt)/2;
    int hx = x + 2, hw = w - 4, xr = x + w - tt;
    int vt = y + tt + 2, vtl = ym - (y + tt) - 2;
    int vb = ym + tt + 2, vbl = (y + h - tt) - (ym + tt) - 2;
    uint16_t hi = mix565(col, 0xFFFF, 6), lo = mix565(col, CLOCK_BLACK, 8);
    if (m & (1<<0)) seg_bar_facet(hx, y,        hw,  tt, false, hi, lo);  /* A */
    if (m & (1<<6)) seg_bar_facet(hx, ym,       hw,  tt, false, hi, lo);  /* G */
    if (m & (1<<3)) seg_bar_facet(hx, y + h-tt, hw,  tt, false, hi, lo);  /* D */
    if (m & (1<<5)) seg_bar_facet(x,  vt,       vtl, tt, true,  hi, lo);  /* F */
    if (m & (1<<1)) seg_bar_facet(xr, vt,       vtl, tt, true,  hi, lo);  /* B */
    if (m & (1<<4)) seg_bar_facet(x,  vb,       vbl, tt, true,  hi, lo);  /* E */
    if (m & (1<<2)) seg_bar_facet(xr, vb,       vbl, tt, true,  hi, lo);  /* C */
}

/* ---- Flip: split-flap card ---------------------------------------------
 * Each digit sits on a rounded dark "flap" card with a horizontal seam across
 * the middle — the classic split-flap desk clock. The card is drawn for every
 * digit (even over a live background, where dark flaps over wallpaper is the
 * whole charm), the numeral (base seg face) on top, then the seam over both. */
#define FLIP_PAD 1
/* Over a live background (GIF/photo/scene) the flap card blends toward the
 * wallpaper instead of sitting as an opaque grey block — the seam (drawn
 * separately, still solid) keeps the split-flap read. On a solid theme
 * background there's nothing under the card to blend with, so it stays the
 * same opaque look as before. */
static void draw_flip_panel(int x, int y, int w, int h, uint16_t card)
{
    draw_round_panel_ex(x - FLIP_PAD, y - 2, w + 2*FLIP_PAD, h + 4, 5, card, !s_ghost_on);
}
static void draw_flip_seam(int x, int y, int w, int h, uint16_t seam)
{
    int sy = y + h/2 - 1, sx = x - FLIP_PAD, sw = w + 2*FLIP_PAD;   /* centre seam */
    if (sx < 0) { sw += sx; sx = 0; }
    if (sx + sw > GW_LCD_WIDTH) sw = GW_LCD_WIDTH - sx;
    if (sw > 0) odroid_overlay_draw_fill_rect(sx, sy, sw, 2, seam);
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
    if (s_outline) {
        int o = s_outline; uint16_t oc = s_outline_col; s_outline = 0;   /* guard recursion */
        draw_pix_digit(d, x - o, y, px, oc, dot); draw_pix_digit(d, x + o, y, px, oc, dot);
        draw_pix_digit(d, x, y - o, px, oc, dot); draw_pix_digit(d, x, y + o, px, oc, dot);
        s_outline = o;
    }
    int inset = dot ? 1 : 0, sz = px - (dot ? 2 : 0);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (DOT5x7[d][r] & (1 << (4 - c)))
                odroid_overlay_draw_fill_rect(x + c*px + inset, y + r*px + inset, sz, sz, col);
}

/* ---- LED: round dot-matrix ---------------------------------------------
 * The same 5x7 glyphs as FACE_PIXEL/DOT but each lit cell is a filled circle
 * — a true dot-matrix LED panel. Disc filled as one span per row (no sqrt),
 * so it is as cheap as the square-pixel face. Shares the s_outline halo. */
static void draw_disc(int cx, int cy, int r, uint16_t col)
{
    for (int dy = -r; dy <= r; dy++) {
        int rr = r*r - dy*dy, sx = 0;
        while ((sx + 1)*(sx + 1) <= rr) sx++;
        int x = cx - sx, wv = 2*sx + 1;
        if (x < 0) { wv += x; x = 0; }
        if (x + wv > GW_LCD_WIDTH) wv = GW_LCD_WIDTH - x;
        if (wv > 0) odroid_overlay_draw_fill_rect(x, cy + dy, wv, 1, col);
    }
}

static void draw_led_digit(int d, int x, int y, int px, uint16_t col)
{
    if (d < 0 || d > PIX_ALL) return;
    if (s_outline) {
        int o = s_outline; uint16_t oc = s_outline_col; s_outline = 0;
        draw_led_digit(d, x-o, y, px, oc); draw_led_digit(d, x+o, y, px, oc);
        draw_led_digit(d, x, y-o, px, oc); draw_led_digit(d, x, y+o, px, oc);
        s_outline = o;
    }
    int r = px/2;
    for (int row = 0; row < 7; row++)
        for (int c = 0; c < 5; c++)
            if (DOT5x7[d][row] & (1 << (4 - c)))
                draw_disc(x + c*px + r, y + row*px + r, r, col);
}

/* Geometry of the big "HH:MM" block per face, so callers can centre extras. */
#define SEG_W    44
#define SEG_H    92
#define SEG_T    10
#define SEG_GAP  10
#define SEG_Y    72   /* digits sit lower: breathing room under the logo bar + balance with the lowered topbar */
#define PIX_PX   9
#define PIX_Y    (SEG_Y + 6)

static int big_time_width(digit_face_t face)
{
    if (!face_is_pixel(face))
        return 4*SEG_W + 3*SEG_GAP + (SEG_T + 2*SEG_GAP);
    return 4*(5*PIX_PX) + 3*PIX_PX + (PIX_PX*3);
}

/* ---- per-face digit dispatch -------------------------------------------
 * Every seg-geometry face is drawn in the same block, so the two big-time
 * drawers just pick the right numeral style here. FACE_FLIP is handled by the
 * cell wrapper (it needs a card behind the numeral), so it renders its numeral
 * with the base seg face. */
static void seg_glyph(digit_face_t face, int d, int x, int y, int w, int h, int t, uint16_t col)
{
    switch (face) {
    case FACE_THIN:    draw_thin_digit(d, x, y, w, h, t, col);    break;
    case FACE_OUTLINE: draw_outline_digit(d, x, y, w, h, t, col); break;
    case FACE_FACET:   draw_facet_digit(d, x, y, w, h, t, col);  break;
    case FACE_LCD:     draw_lcd_digit(d, x, y, w, h, t, col);     break;
    default:           draw_seg_digit(d, x, y, w, h, t, col);     break;   /* SEG7, FLIP numeral */
    }
}

/* One seg-block cell: the ghost-8 backdrop (solid themes only) then the lit
 * numeral. FLIP instead lays a flap card + seam around the numeral, always. */
static void seg_cell(digit_face_t face, int d, int x, int y, int w, int h, int t,
                     uint16_t col, uint16_t ghost, bool gh, bool blank)
{
    if (face == FACE_FLIP) {
        uint16_t seam = mix565(ghost, CLOCK_BLACK, 8);
        draw_flip_panel(x, y, w, h, ghost);            /* dark flap card */
        if (!blank) draw_seg_digit(d, x, y, w, h, t, col);  /* numeral on the card */
        draw_flip_seam(x, y, w, h, seam);              /* seam over both */
        return;
    }
    /* Retro LCD lifts the ghost toward the lit ink so the unlit segments read
     * as a faint "all 8s on" hallmark of a real LCD — faint, not half-lit. */
    uint16_t gcol = (face == FACE_LCD) ? mix565(ghost, col, 4) : ghost;
    if (gh) seg_glyph(face, 8, x, y, w, h, t, gcol);
    if (!blank) seg_glyph(face, d, x, y, w, h, t, col);
}

/* Pixel-family cell glyph: square pixels, inset dots, or round LEDs. */
static void pix_glyph(digit_face_t face, int d, int x, int y, int px, uint16_t col)
{
    if (face == FACE_LED) draw_led_digit(d, x, y, px, col);
    else                  draw_pix_digit(d, x, y, px, col, face == FACE_DOT);
}

/* Draw "HH:MM" centred; when colon=false the colon drops to the ghost shade.
 * Every segment is first drawn in a faint "ghost" colour, the lit ones on
 * top — the unlit-segment look of a real LCD alarm clock. blank_lead hides a
 * leading zero the way segment clocks do (12h "9:41", not "09:41"). */

/* Two-colour core: hours and minutes can differ (the alarm edit view blinks
 * one field by dropping it to the ghost shade). */
/* The subtle "ghost 8" behind each digit (the unlit-LCD look users liked) is
 * drawn only on a SOLID theme background; over a live GIF/scene/ambient it
 * would be a dark block, so render() turns it off there. All faces. */
static bool s_ghost_on = true;

static void draw_big_time_2c(int hh, int mm, bool colon, bool blank_lead,
                             digit_face_t face, uint16_t col_h, uint16_t col_m,
                             uint16_t ghost)
{
    int x = (GW_LCD_WIDTH - big_time_width(face)) / 2;
    int a = hh/10, b = hh%10, c = mm/10, e = mm%10;
    uint16_t cc = colon ? col_h : ghost;   /* blink: colon drops to the dim shade */
    bool gh = s_ghost_on;
    /* Over a live background there IS no matching "dim shade": the ghost is an
     * opaque theme-coloured grey that just floats over the photo/GIF/scene.
     * There the blink-off phase draws nothing at all — a true blink. */
    bool colon_draw = colon || gh;
    if (!face_is_pixel(face)) {
        int w = SEG_W, h = SEG_H, t = SEG_T, gap = SEG_GAP, y = SEG_Y;
        seg_cell(face, a, x, y, w, h, t, col_h, ghost, gh, blank_lead && a == 0);
        x += w+gap;
        seg_cell(face, b, x, y, w, h, t, col_h, ghost, gh, false); x += w+gap;
        /* centre the colon in its slot: the slot is t+3*gap wide, so gap*1.5 on
         * each side (was x+gap, i.e. 2*gap left vs 1*gap right — visibly off) */
        if (colon_draw) {
            odroid_overlay_draw_fill_rect(x+gap+gap/2, y+h/3, t, t, cc);
            odroid_overlay_draw_fill_rect(x+gap+gap/2, y+2*h/3, t, t, cc);
        }
        x += t+2*gap;
        seg_cell(face, c, x, y, w, h, t, col_m, ghost, gh, false); x += w+gap;
        seg_cell(face, e, x, y, w, h, t, col_m, ghost, gh, false);
    } else {
        int px = PIX_PX, dw = 5*px, y = PIX_Y;
        if (gh) pix_glyph(face, PIX_ALL, x, y, px, ghost);
        if (!(blank_lead && a == 0))
            pix_glyph(face, a, x, y, px, col_h);
        x += dw+px;
        if (gh) pix_glyph(face, PIX_ALL, x, y, px, ghost);
        pix_glyph(face, b, x, y, px, col_h); x += dw+px;
        if (colon_draw) {
            odroid_overlay_draw_fill_rect(x+px, y+2*px, px, px, cc);
            odroid_overlay_draw_fill_rect(x+px, y+4*px, px, px, cc);
        }
        x += px*3;
        if (gh) pix_glyph(face, PIX_ALL, x, y, px, ghost);
        pix_glyph(face, c, x, y, px, col_m); x += dw+px;
        if (gh) pix_glyph(face, PIX_ALL, x, y, px, ghost);
        pix_glyph(face, e, x, y, px, col_m);
    }
}

static void draw_big_time(int hh, int mm, bool colon, bool blank_lead,
                          digit_face_t face, uint16_t col, uint16_t ghost)
{
    draw_big_time_2c(hh, mm, colon, blank_lead, face, col, col, ghost);
}

/* ---- icons: baked 1-bit sprites from Lucide (see host/gen_icons.py) --- */
#include "rg_clock_icons.h"

/* Filled crescent moon (solid, not an outline): a disc with an offset disc
 * cut away — only the lit crescent pixels are drawn, so it reads over any
 * background. cx,cy = top-left of an r*2 box. */
/* A bold crescent: a full disc minus a same-size disc shifted up-and-right, so a
 * fat lune remains. The old half-thin sliver aliased into a "broken" blob at
 * this size; keeping most of the disc reads clearly as a moon. */
static void draw_moon(int x, int y, int r, uint16_t col)
{
    int cx = x + r, cy = y + r, ox = r - 1, oy = -1;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int ex = dx - ox, ey = dy - oy;
            if (dx*dx + dy*dy <= r*r && ex*ex + ey*ey > r*r)
                odroid_overlay_draw_fill_rect(cx + dx, cy + dy, 1, 1, col);
        }
}

static void draw_icon(const pix_icon_t *ic, int x, int y, uint16_t col)
{
    for (int r = 0; r < ic->h; r++)
        for (int c = 0; c < ic->w; c++)
            if (ic->rows[r] & (1u << (ic->w - 1 - c)))
                odroid_overlay_draw_fill_rect(x + c, y + r, 1, 1, col);
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
    /* tight 1px drop shadow so the text reads over a live photo/scene/GIF
     * background; solid themes keep the clean flat look */
    if (!s_ghost_on)
        i18n_draw_text_line(x + 1, y + 1, GW_LCD_WIDTH - x, text, CLOCK_BLACK, CLOCK_BLACK, 1);
    i18n_draw_text_line(x, y, GW_LCD_WIDTH - x, text, col, CLOCK_BLACK, 1);
}

/* ---- config + alarms (/clock.cfg) ------------------------------------- */

#define CLOCK_CFG_PATH    "/clock/clock.cfg"   /* lives with /clock/bg.gif */
#define CLOCK_CFG_LEGACY  "/clock.cfg"          /* pre-move location, read-only fallback */
#define MAX_ALARMS      8

typedef struct { uint8_t hour, min, enabled; } alarm_t;

static int      s_theme;
static int      s_face_override = -1;   /* -1 = the theme's face, else FACE_* */
static bool     s_hour24;
static bool     s_dnd;
static int      s_anim;          /* 0 = off, 1 = ambient(retired), 2 = scene, 3 = GIF, 4 = photo */
static int      s_scene;         /* which pixel scene (0..SCENE_COUNT-1) when anim = SCENE */
static bool     s_autodim = true;/* idle backlight auto-dim on the clock face (default on) */

/* Night full-off window, as two independently configurable rows (start hour /
 * end hour) so any combination can be picked, rather than cycling a fixed list
 * of presets. Start cycles Off -> 21:00 -> 22:00 -> 23:00 -> 00:00 -> 01:00
 * (NIGHT_OFF disables the night full-off behaviour ONLY — day half-dim still
 * follows the autodim toggle as usual); End cycles 05:00..09:00 and only
 * matters once Start != Off. Defaults (23:00-07:00) match the original
 * always-on window from before presets/free combination existed, so existing
 * configs upgrade with unchanged behaviour. Persisted as cfg nightstart=/
 * nightend= (literal hour, NIGHT_OFF for "off"); a legacy nightoff= preset
 * (0..3) is migrated to the equivalent pair on load (see clock_config_load). */
#define NIGHT_OFF (-1)
static const int8_t NIGHT_START_HOURS[] = { 21, 22, 23, 0, 1 };
#define NIGHT_START_COUNT (int)(sizeof NIGHT_START_HOURS / sizeof NIGHT_START_HOURS[0])
static const int8_t NIGHT_END_HOURS[] = { 5, 6, 7, 8, 9 };
#define NIGHT_END_COUNT (int)(sizeof NIGHT_END_HOURS / sizeof NIGHT_END_HOURS[0])
static int8_t s_night_start = 23;   /* hour, or NIGHT_OFF */
static int8_t s_night_end   = 7;    /* hour; meaningful only while s_night_start != NIGHT_OFF */

/* index of `hour` in a table, or -1 if not present (used to validate loaded
 * cfg values and to drive the settings rows' PREV/NEXT cycling) */
static int night_start_index(int hour)
{
    for (int i = 0; i < NIGHT_START_COUNT; i++) if (NIGHT_START_HOURS[i] == hour) return i;
    return -1;
}
static int night_end_index(int hour)
{
    for (int i = 0; i < NIGHT_END_COUNT; i++) if (NIGHT_END_HOURS[i] == hour) return i;
    return -1;
}

/* Legacy nightoff= preset (0..3) -> the equivalent {start,end} pair, for
 * migrating configs saved before the two-row free combination existed. */
static const struct { int8_t start, end; } LEGACY_NIGHT_PRESET[4] = {
    { NIGHT_OFF, 7 },   /* 0 = Off (end unused while off) */
    { 22,        6 },
    { 23,        7 },   /* 2 = the old always-on default */
    {  0,        8 },
};

/* The alarm's OWN loudness, independent of the system volume (so turning the
 * system volume down for quiet gaming can't silently mute the morning alarm).
 * Same 0..ODROID_AUDIO_VOLUME_MAX scale/curve as the system volume; 0 means a
 * SILENT alarm — the ring's digit-pulse overlay is still the visual alert.
 * Persisted as cfg alarmvol=. */
static int8_t s_alarm_volume = 6;

/* Synth-beep preset (RG_TONE_*): the non-SD alarm sound, and the SD fallback
 * when no MP3 plays. Selectable on BOTH builds; persisted inside the alarmsnd=
 * cfg key as an ASCII token (matched case-insensitively, so these display
 * labels double as the stored tokens — zero new i18n strings). */
static int8_t s_beep_preset = RG_TONE_BEEP;
static const char *const BEEP_LABELS[RG_TONE_COUNT] = { "Beep", "Beep2", "Chirp", "Siren" };
#define BEEP_LABEL(p) BEEP_LABELS[((p) >= 0 && (p) < RG_TONE_COUNT) ? (p) : 0]

static alarm_t  s_alarms[MAX_ALARMS];
static int      s_alarm_count;
#if CLOCK_SD_MEDIA
/* Chosen basenames from the settings pickers (rescanned on menu open, not held
 * resident). "" = the implicit default: bg.gif / alarm.mp3. "Beep" forces the
 * synth beep. The gif/mp3 modules keep only a POINTER into these persistent
 * buffers, so these are the single resident copy (the launcher's DTCM is very
 * tight). Bounded — the picker skips any name that would not fit. */
static char     s_bgfile[32]   = "";   /* /clock/<name>.gif, "" = bg.gif */
static char     s_alarmsnd[32] = "";   /* /clock/<name>.(mp3|wav), "" = alarm.mp3, "Beep" = synth */
#endif

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
#define ANIM_COUNT 5
#define ANIM_SCENE 2   /* built-in pixel skyline (the mockup's bundled art) */
#define ANIM_GIF   3
#define ANIM_PHOTO 4   /* photo album from /clock/album (borrows shared_files) */

#if CLOCK_SD_MEDIA
/* set once the clock ever borrows shared_files for photos, so the exit path
 * rebuilds the launcher's ROM lists exactly once. */
static bool s_album_used = false;

/* Photo album auto-advance: each photo holds, then a short dip-to-black swaps to
 * the next — never a hard "뿅" cut. Hold time is user-selectable (slow/normal/fast). */
static int s_photo_speed = 1;                          /* 0=slow 1=normal 2=fast */
static const uint32_t PHOTO_HOLD_TBL[3] = { 15000, 8000, 4000 };
#define PHOTO_HOLD_MS (PHOTO_HOLD_TBL[(s_photo_speed >= 0 && s_photo_speed < 3) ? s_photo_speed : 1])
#define PHOTO_FADE_MS 260   /* the dip itself is snappy — a quick 샤라락, not a lingering fade */
static uint32_t s_photo_next   = 0;   /* tick to begin the next advance (0 = off) */
static uint32_t s_fade_start   = 0;   /* tick the dip began (0 = steady) */
static bool     s_fade_swapped = false;
#endif

static void clock_config_load(void)
{
    s_theme = 0; s_face_override = -1;
    s_hour24 = false; s_dnd = false; s_anim = 0; s_scene = 0;
    s_autodim = true;
    s_night_start = 23; s_night_end = 7;
    s_alarm_volume = 6;
    s_beep_preset = RG_TONE_BEEP;
    s_alarm_count = 0;
#if CLOCK_SD_MEDIA
    s_photo_speed = 1;
    s_bgfile[0] = 0; s_alarmsnd[0] = 0;
#endif
    FILE *f = fopen(CLOCK_CFG_PATH, "r");
    if (!f) f = fopen(CLOCK_CFG_LEGACY, "r");   /* migrate: next save writes /clock/ */
    if (!f) return;
    char line[64];
    bool have_nightstart = false;
    int  legacy_nightoff = -1;
    while (fgets(line, sizeof line, f)) {
        int v, en = 1;
        if (sscanf(line, "theme=%d", &v) == 1) { if (v >= 0 && v < THEME_COUNT) s_theme = v; }
        else if (sscanf(line, "face=%d", &v) == 1) { if (v >= -1 && v <= FACE_LAST) s_face_override = v; }
        else if (sscanf(line, "hour24=%d", &v) == 1) s_hour24 = v != 0;
        else if (sscanf(line, "dnd=%d", &v) == 1) s_dnd = v != 0;
        else if (sscanf(line, "anim=%d", &v) == 1) {
            if (v == 1) v = 0;                       /* ambient(1) retired -> off */
#if !CLOCK_SD_MEDIA
            if (v == ANIM_GIF || v == ANIM_PHOTO) v = 0;   /* SD-only media -> off on flash builds */
#endif
            if (v >= 0 && v < ANIM_COUNT) s_anim = v;
        }
        else if (sscanf(line, "scene=%d", &v) == 1) { if (v >= 0) s_scene = v; }  /* draw_scene clamps */
        else if (sscanf(line, "autodim=%d", &v) == 1) s_autodim = v != 0;
        else if (sscanf(line, "nightstart=%d", &v) == 1) {
            have_nightstart = true;
            if (v == NIGHT_OFF || night_start_index(v) >= 0) s_night_start = v;
        }
        else if (sscanf(line, "nightend=%d", &v) == 1) { if (night_end_index(v) >= 0) s_night_end = v; }
        else if (sscanf(line, "nightoff=%d", &v) == 1) { if (v >= 0 && v < 4) legacy_nightoff = v; }
        else if (sscanf(line, "alarmvol=%d", &v) == 1) { if (v >= 0 && v <= ODROID_AUDIO_VOLUME_MAX) s_alarm_volume = v; }
        /* alarmsnd= holds either a synth-preset token (both builds) or, on SD
         * builds, an /clock sound-file basename. Set the preset from a token on
         * either build; keep the raw string only where files are supported. */
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
    /* migrate an old single-preset nightoff= into the new start/end pair —
     * only when the file never had the new keys, so a config already saved
     * under the new scheme is never overwritten by a stale nightoff= line. */
    if (!have_nightstart && legacy_nightoff >= 0) {
        s_night_start = LEGACY_NIGHT_PRESET[legacy_nightoff].start;
        s_night_end   = LEGACY_NIGHT_PRESET[legacy_nightoff].end;
    }
}

static void clock_config_save(void)
{
    rg_storage_mkdir("/clock");   /* harmless if it already exists */
    FILE *f = fopen(CLOCK_CFG_PATH, "w");
    if (!f) return;
    fprintf(f, "theme=%d\n", s_theme);
    fprintf(f, "face=%d\n", s_face_override);
    fprintf(f, "hour24=%d\n", s_hour24 ? 1 : 0);
    fprintf(f, "dnd=%d\n", s_dnd ? 1 : 0);
    fprintf(f, "anim=%d\n", s_anim);
    fprintf(f, "scene=%d\n", s_scene);
    fprintf(f, "autodim=%d\n", s_autodim ? 1 : 0);
    fprintf(f, "nightstart=%d\n", s_night_start);
    fprintf(f, "nightend=%d\n", s_night_end);
    fprintf(f, "alarmvol=%d\n", s_alarm_volume);
#if CLOCK_SD_MEDIA
    fprintf(f, "photospeed=%d\n", s_photo_speed);
    if (s_bgfile[0])   fprintf(f, "bgfile=%s\n", s_bgfile);
    if (s_alarmsnd[0]) fprintf(f, "alarmsnd=%s\n", s_alarmsnd);
#else
    /* flash build: the alarm sound is a synth preset — persist it as a token */
    fprintf(f, "alarmsnd=%s\n", BEEP_LABEL(s_beep_preset));
#endif
    for (int i = 0; i < s_alarm_count; i++)   /* disabled alarms persist too */
        fprintf(f, "alarm=%02d%02d,%d\n", s_alarms[i].hour, s_alarms[i].min,
                s_alarms[i].enabled ? 1 : 0);
    fclose(f);

    /* keep the resident all-state next-alarm cache in step with the file */
    rg_alarm_cache_refresh();
}

/* ---- exports for the resident all-state alarm cache (rg_alarm.c) -------
 * These reuse clock_config_load()'s parser so there is exactly one place that
 * understands clock.cfg. clock_config_load() only writes the file-static clock
 * state, which rg_clock_show() reloads on entry anyway, so calling it here is
 * side-effect-free for the launcher. */
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
    /* an SD file name is not a synth preset — the in-place ring is beep-only
     * (the MP3 decoder needs the emulator's RAM), so fall back to the preset. */
    int t = rg_tone_preset_from_token(s_alarmsnd);
    if (t >= 0) p = t;
#endif
    if (preset) *preset = p;
    if (volume) *volume = s_alarm_volume;
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
#define CLOCK_UI_HIDE_MS 8000u   /* idle time before the mode pager + hint fade away */
#define CLOCK_DIM_IDLE_MS 15000u /* idle time on the clock face before the backlight auto-dims/turns off */
#define CLOCK_DIM_FLOOR   16u    /* never dim below this raw level, even off a very low user brightness */
static uint32_t s_snooze_tick = 0;   /* HAL tick when a snoozed alarm re-rings */

/* Idle backlight decision — pure logic, so it is unit-testable
 * (tests/test_clock_more.c). Applies only on the clock face, never over a
 * running timer/pomodoro/stopwatch and never while the alarm rings:
 *   CLOCK_BL_FULL — awake, or the idle threshold hasn't been reached
 *   CLOCK_BL_DIM  — idle during the day: half the user's brightness (floor-clamped)
 *   CLOCK_BL_OFF  — idle during the night window: backlight fully off, like a
 *                   bedside clock
 * Ringing always forces FULL, including a wake from night-OFF. */
typedef enum { CLOCK_BL_FULL = 0, CLOCK_BL_DIM, CLOCK_BL_OFF } clock_backlight_t;

/* Same minutes-of-day wraparound idiom as alarm_fired_in_window() elsewhere in
 * this file: "minutes since window start, modulo a day" compared against the
 * window's span handles the midnight crossing without a branch per boundary. */
static bool clock_in_night_window(int minute_of_day, int start_min, int end_min)
{
    int day = 24 * 60;
    int since_start = ((minute_of_day - start_min) % day + day) % day;
    int span = ((end_min - start_min) % day + day) % day;
    return since_start < span;
}

/* Half of the user's current configured brightness, clamped so dimming alone
 * never goes fully dark (lcd_backlight_set takes a raw 0-255 DAC level). */
static uint8_t clock_dim_level(uint8_t user_raw)
{
    uint8_t half = (uint8_t)(user_raw / 2);
    return half < CLOCK_DIM_FLOOR ? CLOCK_DIM_FLOOR : half;
}

/*
 * Priority, highest first:
 *   1. ringing                                    -> FULL (never dim/hide an alarm)
 *   2. autodim off / not the clock face / not idle yet -> FULL
 *   3. night window enabled (night_start_min >= 0) AND currently inside it
 *                                                  -> OFF (dark-bedroom bedside
 *      clock; applies even while charging — a charger left plugged in
 *      overnight must not light the room up)
 *   4. charging (desk clock on permanent power)    -> FULL (the daytime
 *      half-dim exists to save battery; skip it when nothing is being spent)
 *   5. otherwise                                   -> DIM (half brightness)
 */
static clock_backlight_t clock_should_dim(clock_mode_t mode, bool ringing, bool autodim,
                                           uint32_t idle_ms, int minute_of_day,
                                           int night_start_min, int night_end_min, bool charging)
{
    if (ringing) return CLOCK_BL_FULL;   /* alarm always forces full brightness, even waking from night-OFF */
    if (!autodim || mode != MODE_CLOCK || idle_ms < CLOCK_DIM_IDLE_MS) return CLOCK_BL_FULL;
    if (night_start_min >= 0 && clock_in_night_window(minute_of_day, night_start_min, night_end_min))
        return CLOCK_BL_OFF;
    if (charging) return CLOCK_BL_FULL;   /* desk clock on permanent power: skip the battery-saving day dim */
    return CLOCK_BL_DIM;
}

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
#include "rg_clock_scenes.inc"   /* 16 procedural scenes + srect/itri/shash helpers */

/* 0) city — the original night skyline (kept as scene index 0) */
static void scene_city(uint32_t now, const clock_theme_t *t)
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

/* pixel-scene registry — s_scene selects one; all are procedural (0 RAM). */
typedef void (*scene_fn)(uint32_t, const clock_theme_t *);
/* A curated 8-scene set (16->10->8; Rain/Matrix/Forest delisted 0707 to fund the
 * all-state alarm — Rain overlapped Snow, Matrix fought the digits). Grid Pulse is reused
 * as the alarm ring effect so it's not selectable. Several scenes remain defined
 * in the .inc but delisted here (gc-sections drops them): mountains, desert,
 * meteor, bubbles, fireworks, clouds, and the newer equalizer/plasma/helix —
 * any can be re-listed when there's budget. */
static const scene_fn SCENES[] = {
    scene_city, scene_ocean, scene_starfield, scene_synthwave,
    scene_snow, scene_aurora, scene_campfire,
};
static const char *const SCENE_NAMES[] = {
    "City", "Ocean", "Starfield", "Synthwave",
    "Snow", "Aurora", "Campfire",
};
#define SCENE_COUNT ((int)(sizeof(SCENES) / sizeof(SCENES[0])))

static void draw_scene(uint32_t now, const clock_theme_t *t)
{
    int idx = (s_scene >= 0 && s_scene < SCENE_COUNT) ? s_scene : 0;
    SCENES[idx](now, t);
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
 */
static void draw_topbar(clock_mode_t mode, bool show_pager)
{
    const clock_theme_t *t = TH();
    if (!s_ghost_on)   /* 1px drop shadow behind the logo over a live background */
        odroid_overlay_draw_logo(10, 9, RG_LOGO_GNW, CLOCK_BLACK);
    odroid_overlay_draw_logo(9, 8, RG_LOGO_GNW, t->ink);
    /* EXACTLY the launcher header's battery spot (gui.c: W-28, y=17) — the
     * clock sat 6px left / 1px low of it and the mismatch showed when
     * switching between the launcher and the clock */
    odroid_overlay_draw_battery(odroid_input_read_battery(), GW_LCD_WIDTH - 28, 17);
    if (s_dnd) draw_moon(GW_LCD_WIDTH - 58, 16, 7, t->ink);
    /* The logo/battery stay; the mode pager auto-hides after a few idle seconds
     * (any key brings it back) so the clock face reads clean. */
    if (!show_pager) return;
    /* No text title — the mode reads from the icon pager alone (current one
     * lit in the accent), with <> strokes hinting the L/R switch. Inactive
     * icons use a soft, LIGHT tint — a heavy dark grey looked tacky over a
     * bright photo background. */
    uint16_t dim = mix565(t->scr, t->ink, 9);
    int pw = MODE_COUNT*17 - 4;
    int dx = (GW_LCD_WIDTH - pw) / 2, iy = 15;  /* lowered to sit on the battery's row */
    /* All four mode glyphs are 13px tall on the same row (iy..iy+12, centre iy+6).
     * The chevrons are 7px, so y = iy - 3 sits them on that same centre. */
    draw_icon(&PIX_CHEV_L, dx - 15, iy + 3, dim);
    draw_icon(&PIX_CHEV_R, dx + pw + 9, iy + 3, dim);
    for (int i = 0; i < MODE_COUNT; i++, dx += 17) {
        uint16_t c = (i == (int)mode) ? t->alarm : dim;
        switch (i) {
        case MODE_CLOCK:     draw_icon(&PIX_CLOCK,     dx, iy, c); break;
        case MODE_POMODORO:  draw_icon(&PIX_TOMATO,    dx, iy, c); break;
        case MODE_TIMER:     draw_icon(&PIX_HOURGLASS, dx + 1, iy, c); break;
        default:             draw_icon(&PIX_STOPWATCH, dx, iy, c); break;
        }
    }
}

/* Bottom hint: ALWAYS visible, in the firmware's default 8px font (crisp,
 * matches the rest of the OS chrome — the 12px serif looked broken here),
 * sitting on a rounded pill so it reads as one quiet control strip.
 *
 * NOTE the device blitters do NOT clip: a rect or text cell that crosses the
 * right edge wraps into the next row's left side (the "crumbs" beside the
 * old hint bar). Everything here is therefore clamped to the screen. */
/* blend=false: same opaque rounded fill every caller originally got. blend=true
 * (Flip's card over a live background) mixes col INTO the framebuffer pixels
 * already there instead of overwriting them, so wallpaper shows through the
 * card — same rounded-rect geometry either way, just a per-pixel read+blend
 * instead of one fill_rect per row when translucent. */
static void draw_round_panel_ex(int x, int y, int w, int h, int r, uint16_t col, bool blend)
{
    uint16_t *fb = blend ? (uint16_t *)lcd_get_active_buffer() : NULL;
    for (int j = 0; j < h; j++) {
        int dy = (j < r) ? r - 1 - j : (j >= h - r ? j - (h - r - 1) : -1);
        int inset = 0;
        if (dy >= 0) {   /* largest k with k^2 + dy^2 <= r^2 = quarter circle */
            inset = r;
            for (int k = r; k >= 0; k--)
                if (k*k + dy*dy <= r*r) { inset = r - k; break; }
        }
        int rx = x + inset, rw = w - 2*inset, ry = y + j;
        if (rx < 0) { rw += rx; rx = 0; }
        if (rx + rw > GW_LCD_WIDTH) rw = GW_LCD_WIDTH - rx;
        if (rw <= 0 || ry < 0 || ry >= GW_LCD_HEIGHT) continue;
        if (!blend) { odroid_overlay_draw_fill_rect(rx, ry, rw, 1, col); continue; }
        for (int i = 0; i < rw; i++) {
            int idx = ry * GW_LCD_WIDTH + rx + i;
            fb[idx] = mix565(fb[idx], col, 8);   /* ~50/50 — wallpaper still reads through */
        }
    }
}

static void draw_round_panel(int x, int y, int w, int h, int r, uint16_t col)
{ draw_round_panel_ex(x, y, w, h, r, col, false); }

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

/* Fill the whole framebuffer with one colour writing 32 bits (two pixels) per
 * store — halves the memory writes vs a per-pixel loop. The buffer is a full
 * 320x240 (even count) and 4-byte aligned, so the word loop is exact. */
static inline void fb_fill_screen(uint16_t *fb, uint16_t c)
{
    uint32_t c2 = (uint32_t)c | ((uint32_t)c << 16);
    uint32_t *p = (uint32_t *)fb;
    for (int i = 0; i < (GW_LCD_WIDTH * GW_LCD_HEIGHT) / 2; i++) p[i] = c2;
}


#if CLOCK_SD_MEDIA
/* Dip-to-black progress: 0 at the ends (steady) .. 16 fully black at the midpoint
 * of the PHOTO_FADE_MS transition. Applied to the photo before the digits go on. */
static int photo_fade_darkness(uint32_t now)
{
    if (!s_fade_start) return 0;
    uint32_t el = now - s_fade_start, half = PHOTO_FADE_MS / 2;
    uint32_t d = (el < half) ? el : (el < PHOTO_FADE_MS ? PHOTO_FADE_MS - el : 0);
    int v = (int)(d * 16 / half);
    return v > 16 ? 16 : v;
}
#endif

/* Alarm ring overlay — drawn AFTER the background + face so the pulse reads
 * over ANY background (GIF, photo, busy pixel scene). Two layers guarantee
 * contrast on both bright and dark content: (1) a whole-frame scrim that THROBS
 * with the 2.5 Hz alarm beat, so even a bright photo visibly darkens on the
 * beat; (2) an outward ripple of accent dots, each drawn with a black halo so a
 * bright dot still reads on bright content and the halo frames it on dark
 * content. The blitter has NO clipping, so every rect stays inside 320x240. */
#define RING_GRID   12
#define RING_SPAN   150   /* how far the ring front travels before it restarts */
static void draw_ring_overlay(uint16_t *fb, uint32_t now, const clock_theme_t *t)
{
    int dk = ((now / 200) & 1) ? 6 : 3;            /* throb 6/16 <-> 3/16 toward black */
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++)
        fb[i] = mix565(fb[i], CLOCK_BLACK, dk);

    int cx = GW_LCD_WIDTH / 2, cy = GW_LCD_HEIGHT / 2;
    int pulse = (int)((now / 24) % RING_SPAN);     /* ring front sweeps outward */
    uint16_t bright = t->alarm;
    for (int y = RING_GRID; y < GW_LCD_HEIGHT - RING_GRID; y += RING_GRID)
        for (int x = RING_GRID; x < GW_LCD_WIDTH - RING_GRID; x += RING_GRID) {
            int a = x - cx, b = y - cy; if (a < 0) a = -a; if (b < 0) b = -b;
            int d = (a > b) ? a + b/2 : b + a/2;   /* octagonal distance approx */
            int diff = d - pulse; if (diff < 0) diff = -diff;
            if (diff >= 7) continue;               /* draw only the moving ring front */
            odroid_overlay_draw_fill_rect(x - 1, y - 1, 4, 4, CLOCK_BLACK);  /* halo */
            int sz = (diff < 3) ? 3 : 2;
            odroid_overlay_draw_fill_rect(x, y, sz, sz, bright);            /* core */
        }
}

static void render_clock(uint32_t now, bool alarm_firing)
{
    const clock_theme_t *t = TH();
    int hh = GW_GetCurrentHour(), mm = GW_GetCurrentMinute();
    bool colon = GW_GetCurrentSubSeconds() <= 127;

    /* date + weekday, centred UNDER the top bar (was colliding with it) */
    char date[48];
    snprintf(date, sizeof date, "%02d/%02d %s", GW_GetCurrentMonth(), GW_GetCurrentDay(), weekday_str());
    draw_centered_i18n(42, date, t->ink);

    /* the alarm's background effect (grid-pulse) is drawn in the main loop's
     * background layer while ringing — nothing extra to draw here */

    /* big time. When the alarm is ringing the digits PULSE between ink and
     * the accent (~2.5 Hz): a clear alarm signal that sits on top of the
     * GIF/ambient background instead of a full-screen flash covering it. */
    int dh = hh;
    if (!s_hour24) { dh = hh % 12; if (dh == 0) dh = 12; }
    uint16_t timecol = t->ink;
    if (alarm_firing && ((now / 200) & 1)) timecol = t->alarm;
    digit_face_t face = cur_face();
    /* over a live photo/scene (no ghost) give the digits a dark halo so they read
     * over any background; solid themes keep the clean ghost look (no outline). */
    if (!s_ghost_on) { s_outline = 2; s_outline_col = CLOCK_BLACK; }
    draw_big_time(dh, mm, colon, !s_hour24, face, timecol, mix565(t->scr, t->ink, 2));
    s_outline = 0;

    /* AM/PM tucked to the RIGHT of the digits at their baseline (G&W style),
     * not a whole centred line of its own */
    if (!s_hour24) {
        int x = (GW_LCD_WIDTH + big_time_width(face)) / 2 + 6;
        int yb = !face_is_pixel(face) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
        const char *ap = hh < 12 ? curr_lang->s_AM : curr_lang->s_PM;
        if (!s_ghost_on)   /* 1px drop shadow over a live background, like the other text */
            i18n_draw_text_line(x + 1, yb + 1, GW_LCD_WIDTH - x, ap, CLOCK_BLACK, CLOCK_BLACK, 1);
        i18n_draw_text_line(x, yb, GW_LCD_WIDTH - x, ap, t->ink, CLOCK_BLACK, 1);
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
            /* centre the bell + text as ONE block (16px = icon + gap); centring
             * only the text left the pair 8px off-centre */
            int tw = i18n_get_text_width(line);
            int bx = (GW_LCD_WIDTH - (tw + 16)) / 2;
            if (bx < 0) bx = 0;
            draw_icon(&PIX_BELL, bx, STATUS_Y + 1, alcol);
            if (!s_ghost_on)   /* same 1px drop shadow as draw_centered_i18n */
                i18n_draw_text_line(bx + 17, STATUS_Y + 1, GW_LCD_WIDTH - bx - 17, line, CLOCK_BLACK, CLOCK_BLACK, 1);
            i18n_draw_text_line(bx + 16, STATUS_Y, GW_LCD_WIDTH - bx - 16, line, alcol, CLOCK_BLACK, 1);
        }
    }

#if CLOCK_SD_MEDIA
    /* GIF selected but not showing? put the reason on screen (device = log) */
    if (s_anim == ANIM_GIF && !clock_gif_ready()) {
        char msg[80]; snprintf(msg, sizeof msg, "GIF: %s", clock_gif_diag());
        draw_centered_i18n(STATUS_Y + 22, msg, t->alarm);
    }
#endif
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
    uint16_t stc = s_pomo_on_break ? t->alarm : t->ink;
    int sx = (GW_LCD_WIDTH - i18n_get_text_width(st)) / 2;
    draw_icon(s_pomo_on_break ? &PIX_MUG : &PIX_TOMATO, sx - 18, STATUS_Y + 1, stc);
    draw_centered_i18n(STATUS_Y, st, stc);
}

static void render_timer(uint32_t now)
{
    bool colon = (now / 500) & 1;
    render_mmss(s_timer.remaining_ms, TH()->ink, s_timer.state != RUN_RUNNING ? true : colon);
}

/* Stopwatch: MM:SS.cc as ONE big six-digit display — physically big, not a
 * small side annotation. Segments scale down so all six digits + colon +
 * decimal point fit the width; the ghost treatment stays. */
static void render_stopwatch(uint32_t now)
{
    (void)now;
    const clock_theme_t *t = TH();
    uint16_t col = t->ink, ghost = mix565(t->scr, t->ink, 2);
    bool gh = s_ghost_on;
    uint32_t ms = s_watch.elapsed_ms;
    int d[6] = { (int)((ms/60000) % 100) / 10, (int)((ms/60000) % 100) % 10,
                 (int)((ms/1000)  % 60)  / 10, (int)((ms/1000)  % 60)  % 10,
                 (int)((ms/10)    % 100) / 10, (int)((ms/10)    % 100) % 10 };
    digit_face_t face = cur_face();

    if (!face_is_pixel(face)) {
        const int w = 32, h = 68, tt = 8, g = 6, sep = tt + 2*g;
        int total = 6*w + 3*g + 2*sep;
        int x = (GW_LCD_WIDTH - total) / 2, y = SEG_Y + 12;
        for (int i = 0; i < 6; i++) {
            seg_cell(face, d[i], x, y, w, h, tt, col, ghost, gh, false);
            x += w + ((i == 1 || i == 3) ? 0 : g);
            if (i == 1) {           /* colon */
                odroid_overlay_draw_fill_rect(x+g, y+h/3, tt, tt, col);
                odroid_overlay_draw_fill_rect(x+g, y+2*h/3, tt, tt, col);
                x += sep;
            } else if (i == 3) {    /* decimal point, at the baseline */
                odroid_overlay_draw_fill_rect(x+g, y+h-tt, tt, tt, col);
                x += sep;
            }
        }
    } else {
        const int px = 7, dw = 5*px, g = px, sep = px*3;
        int total = 6*dw + 3*g + 2*sep;
        int x = (GW_LCD_WIDTH - total) / 2, y = PIX_Y + 10;
        for (int i = 0; i < 6; i++) {
            if (gh) pix_glyph(face, PIX_ALL, x, y, px, ghost);
            pix_glyph(face, d[i], x, y, px, col);
            x += dw + ((i == 1 || i == 3) ? 0 : g);
            if (i == 1) {
                odroid_overlay_draw_fill_rect(x+px, y+2*px, px, px, col);
                odroid_overlay_draw_fill_rect(x+px, y+4*px, px, px, col);
                x += sep;
            } else if (i == 3) {
                odroid_overlay_draw_fill_rect(x+px, y+6*px, px, px, col);
                x += sep;
            }
        }
    }
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

/* forward decls: the Alarm Sound / DND rows folded into this popup (see
 * ALARM_IDX_* below) reuse the same callbacks the main clock menu used to
 * call directly for those rows; the callbacks themselves are defined later,
 * next to the rest of the settings-menu callbacks. */
static bool cb_dnd(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r);
#if CLOCK_SD_MEDIA
static bool cb_alarmsnd(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r);
#endif

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
    fb_fill_screen(fb, t->scr);

    char title[64];
    snprintf(title, sizeof title, "%s", curr_lang->s_Clock_Alarms);
    int tw = i18n_get_text_width(title);
    draw_icon(&PIX_BELL, (GW_LCD_WIDTH - tw) / 2 - 16, 12, t->alarm);
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
        int yb = !face_is_pixel(face) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
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

/* Row layout: [alarms...][+Add Alarm][Alarm Sound][DND][Done]. The Alarm Sound
 * row is on BOTH builds now: SD builds pick synth presets + /clock sound files,
 * flash builds pick the synth presets only (non-SD units still get to choose a
 * sound). Alarm Sound and DND used to be their own rows on the main clock menu;
 * folding them in here is what let the main popup drop under the no-scroll row
 * budget (see clock_settings_menu). */
#define ALARM_IDX_SOUND(cnt) ((cnt) + 1)
#define ALARM_IDX_DND(cnt)   ((cnt) + 2)
#define ALARM_EXTRA_ROWS     4   /* +Add, Alarm Sound, DND, Done */
#define ALARM_IDX_ADD(cnt)   (cnt)
#define ALARM_IDX_DONE(cnt)  ((cnt) + ALARM_EXTRA_ROWS - 1)

/* Alarm-sound row value/cycle, build-agnostic so the row code stays simple:
 * SD builds delegate to the preset+file picker; flash builds cycle the four
 * synth presets directly (s_beep_preset). */
static void alarm_sound_str(char *out, size_t n)
{
#if CLOCK_SD_MEDIA
    odroid_dialog_choice_t tmp = {0}; char val[40] = ""; tmp.value = val;
    cb_alarmsnd(&tmp, ODROID_DIALOG_INIT, 0);
    snprintf(out, n, "%s", val);
#else
    snprintf(out, n, "%s", BEEP_LABEL(s_beep_preset));
#endif
}
static void alarm_sound_cycle(odroid_dialog_event_t ev)
{
#if CLOCK_SD_MEDIA
    odroid_dialog_choice_t tmp = {0}; char val[40] = ""; tmp.value = val;
    cb_alarmsnd(&tmp, ev, 0);
#else
    if (ev == ODROID_DIALOG_PREV) s_beep_preset = (s_beep_preset == 0) ? RG_TONE_COUNT - 1 : s_beep_preset - 1;
    else                          s_beep_preset = (int8_t)((s_beep_preset + 1) % RG_TONE_COUNT);
#endif
}

/* A proper popup: clean solid base + bordered panel, repainted whole every
 * frame. (The old version re-scrimmed whatever was already on the swap
 * buffer, so each repaint stacked another darkening layer + stale rows.) */
static void render_alarm_setup(int sel)
{
    const clock_theme_t *t = TH();
    uint16_t *fb = lcd_get_active_buffer();
    fb_fill_screen(fb, t->scr);
    /* keep the user's living background behind the panel instead of a hard
     * wipe (device feedback: the editor looked detached from the clock).
     * One background frame per dirty-render + a dark scrim so the panel
     * still dominates. */
    if (s_anim != 0) {
        uint32_t bt = HAL_GetTick();
#if CLOCK_SD_MEDIA
        if (s_anim == ANIM_GIF && clock_gif_ready()) clock_gif_blit(fb, bt);
        else if (s_anim == ANIM_PHOTO && clock_album_ready())
            memcpy(fb, clock_album_current(), (size_t)GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);
        else
#endif
        if (s_anim == ANIM_SCENE) draw_scene(bt, t);
        for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++)
            fb[i] = mix565(fb[i], CLOCK_BLACK, 9);
    }

    int rows = s_alarm_count + ALARM_EXTRA_ROWS, rh = 18;
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
        } else if (i == ALARM_IDX_ADD(s_alarm_count)) {
            snprintf(line, sizeof line, "+ %s", curr_lang->s_Clock_Add_Alarm);
        } else if (i == ALARM_IDX_SOUND(s_alarm_count)) {
            char val[40]; alarm_sound_str(val, sizeof val);
            snprintf(line, sizeof line, "%s        %s", curr_lang->s_Clock_Alarm_Sound, val);
        } else if (i == ALARM_IDX_DND(s_alarm_count)) {
            odroid_dialog_choice_t tmp = {0}; char val[12] = "";
            tmp.value = val; cb_dnd(&tmp, ODROID_DIALOG_INIT, 0);
            snprintf(line, sizeof line, "%s        %s", curr_lang->s_Clock_DND, tmp.value);
        } else {
            snprintf(line, sizeof line, "%s", curr_lang->s_Clock_Done);
        }
        draw_centered_i18n(y, line, col);
    }
    draw_hintbar(curr_lang->s_Clock_Hint_Editor);
}

static void alarm_delete_at(int sel)
{
    for (int i = sel; i < s_alarm_count - 1; i++) s_alarms[i] = s_alarms[i+1];
    s_alarm_count--;
}

/* days in a Gregorian month (mon0 = 0..11), leap-aware */
static int days_in_month(int year, int mon0)
{
    static const uint8_t d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mon0 == 1) { bool leap = (year%4==0 && (year%100!=0 || year%400==0)); return leap ? 29 : 28; }
    return d[mon0 & 11];
}

/* Full set-time screen: a YYYY-MM-DD line (fields 0..2) above the big HH:MM
 * (fields 3..4, reusing the alarm editor's blink), the active field pulsing. */
static void render_datetime_edit(const struct tm *tm, int field, bool blink_off)
{
    const clock_theme_t *t = TH();
    uint16_t *fb = lcd_get_active_buffer();
    fb_fill_screen(fb, t->scr);
    draw_centered_i18n(30, curr_lang->s_Clock_Set_Time, t->alarm);

    char yy[8], mo[8], dd[8];
    snprintf(yy, sizeof yy, "%04d", tm->tm_year + 1900);
    snprintf(mo, sizeof mo, "%02d", tm->tm_mon + 1);
    snprintf(dd, sizeof dd, "%02d", tm->tm_mday);
    const char *seg[5] = { yy, "-", mo, "-", dd };
    int fld[5] = { 0, -1, 1, -1, 2 };
    int total = 0; for (int i = 0; i < 5; i++) total += i18n_get_text_width(seg[i]);
    int x = (GW_LCD_WIDTH - total) / 2, y = 52;
    for (int i = 0; i < 5; i++) {
        uint16_t col = (fld[i] == field) ? (blink_off ? mix565(t->scr, t->ink, 4) : t->alarm) : t->ink;
        i18n_draw_text_line(x, y, GW_LCD_WIDTH - x, seg[i], col, CLOCK_BLACK, 1);
        x += i18n_get_text_width(seg[i]);
    }

    int dh = tm->tm_hour;
    if (!s_hour24) { dh = tm->tm_hour % 12; if (dh == 0) dh = 12; }
    digit_face_t face = cur_face();
    uint16_t ghost = mix565(t->scr, t->ink, 2);
    uint16_t ch = (field == 3 && blink_off) ? ghost : t->ink;
    uint16_t cm = (field == 4 && blink_off) ? ghost : t->ink;
    draw_big_time_2c(dh, tm->tm_min, true, !s_hour24, face, ch, cm, ghost);
    if (!s_hour24) {
        int xx = (GW_LCD_WIDTH + big_time_width(face)) / 2 + 6;
        int yb = !face_is_pixel(face) ? SEG_Y + SEG_H - 12 : PIX_Y + 7*PIX_PX - 12;
        i18n_draw_text_line(xx, yb, GW_LCD_WIDTH - xx,
                            tm->tm_hour < 12 ? curr_lang->s_AM : curr_lang->s_PM, t->ink, CLOCK_BLACK, 1);
    }
    draw_hintbar(curr_lang->s_Clock_Hint_Edit);
}

/* Set the device clock (date + time). LEFT/RIGHT pick a field (year, month, day,
 * hour, minute); UP/DOWN adjust; A saves to the RTC, B cancels. */
static void clock_edit_time(void)
{
    struct tm tm;
    GW_GetUnixTM(&tm);
    if (tm.tm_year < 70) tm.tm_year = 70;   /* floor at 1970 */
    int field = 0; bool dirty = true; uint32_t last_phase = ~0u;
    odroid_gamepad_state_t k, prev = {0};
    odroid_input_read_gamepad(&prev);

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();

        if (pressed(&k, &prev, ODROID_INPUT_A)) { tm.tm_sec = 0; GW_SetUnixTM(&tm); break; }
        if (pressed(&k, &prev, ODROID_INPUT_B)) break;
        if (pressed(&k, &prev, ODROID_INPUT_LEFT))  { field = (field == 0) ? 4 : field - 1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_RIGHT)) { field = (field + 1) % 5; dirty = true; }
        bool up = pressed(&k, &prev, ODROID_INPUT_UP), dn = pressed(&k, &prev, ODROID_INPUT_DOWN);
        if (up || dn) {
            switch (field) {
            case 0: tm.tm_year += up ? 1 : -1;
                    if (tm.tm_year < 70) tm.tm_year = 70; if (tm.tm_year > 199) tm.tm_year = 199; break;
            case 1: tm.tm_mon = (tm.tm_mon + (up ? 1 : 11)) % 12; break;
            case 2: { int dim = days_in_month(tm.tm_year + 1900, tm.tm_mon);
                      tm.tm_mday = ((tm.tm_mday - 1 + (up ? 1 : dim - 1)) % dim) + 1; } break;
            case 3: tm.tm_hour = (tm.tm_hour + (up ? 1 : 23)) % 24; break;
            case 4: tm.tm_min  = (tm.tm_min  + (up ? 1 : 59)) % 60; break;
            }
            int dim = days_in_month(tm.tm_year + 1900, tm.tm_mon);
            if (tm.tm_mday > dim) tm.tm_mday = dim;   /* clamp Feb 29 -> 28, etc. */
            dirty = true;
        }

        uint32_t phase = now / 500;
        if (dirty || phase != last_phase) {
            dirty = false; last_phase = phase;
            render_datetime_edit(&tm, field, (phase & 1) != 0);
            lcd_swap(); lcd_sleep_while_swap_pending();
        }
        prev = k;
        HAL_Delay(40);
    }
    do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); } while (k.bitmask);
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
        int rows = s_alarm_count + ALARM_EXTRA_ROWS;

        if (pressed(&k, &prev, ODROID_INPUT_B)) break;
        if (pressed(&k, &prev, ODROID_INPUT_UP))   { sel = (sel == 0) ? rows-1 : sel-1; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_DOWN)) { sel = (sel+1) % rows; dirty = true; }
        if (pressed(&k, &prev, ODROID_INPUT_LEFT) || pressed(&k, &prev, ODROID_INPUT_RIGHT)) {
            /* Alarm Sound / DND rows: L/R cycles the value, same as the
             * generic dialog rows they used to be on the main clock menu. */
            odroid_dialog_event_t ev = pressed(&k, &prev, ODROID_INPUT_LEFT) ? ODROID_DIALOG_PREV : ODROID_DIALOG_NEXT;
            if (sel == ALARM_IDX_SOUND(s_alarm_count)) {
                alarm_sound_cycle(ev); dirty = true;
            } else
            if (sel == ALARM_IDX_DND(s_alarm_count)) {
                odroid_dialog_choice_t tmp = {0}; char val[12] = "";
                tmp.value = val; cb_dnd(&tmp, ev, 0); dirty = true;
            }
        }
        if (pressed(&k, &prev, ODROID_INPUT_A)) {
            if (sel < s_alarm_count) {
                /* edit in the full-screen clone view; B there restores */
                alarm_t backup = s_alarms[sel];
                if (!alarm_edit_view(&s_alarms[sel]))
                    s_alarms[sel] = backup;
                odroid_input_read_gamepad(&prev);
            } else if (sel == ALARM_IDX_ADD(s_alarm_count)) {
                if (s_alarm_count < MAX_ALARMS) {
                    s_alarms[s_alarm_count] = (alarm_t){ 7, 0, 1 };
                    sel = s_alarm_count; s_alarm_count++;
                    /* cancel on a fresh add = the alarm never existed. No
                     * cursor clamp here: sel == s_alarm_count is the still-valid
                     * "+ Add" row, so the highlight stays put (unlike the
                     * START-delete path, which may drop off a real last row). */
                    if (!alarm_edit_view(&s_alarms[sel]))
                        alarm_delete_at(sel);
                    odroid_input_read_gamepad(&prev);
                }
            } else if (sel == ALARM_IDX_DONE(s_alarm_count)) {
                break;
            }
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
    { "Midnight", "Amber", "Green LCD", "Ivory", "Ember", "Aqua", "Neon", "Slate",
      "Mint", "Sakura", "Arctic", "Forest", "OLED", "Term" };
/* Face names are ASCII and NOT translated (same as the theme labels) — the
 * settings dialog shows them verbatim. Order matches digit_face_t. */
static const char *const FACE_NAME[FACE_COUNT] = {
    "7-seg", "Pixel", "Dot", "Thin", "Outline", "Facet", "Flip", "LED", "LCD",
};
static char v_theme[24], v_face[20], v_fmt[8], v_anim[44], v_scene[24],
            v_autodim[12], v_vol[ODROID_AUDIO_VOLUME_MAX + 2], v_settime[4], v_alarms[4], v_exit[4];
/* v_autodim also backs the main-menu "절전" group row's On/Off summary
 * (cb_powersave_group) — safe to share since the main dialog is always fully
 * closed before clock_powersave_menu() opens (never rendered together).
 * v_dnd is gone: DND moved into the alarm popup, which uses its own stack
 * buffer (see ALARM_IDX_DND in clock_alarm_setup). v_night_start / v_night_end
 * / v_bright live on clock_settings_menu()'s and clock_powersave_menu()'s own
 * STACKs (with the SD pickers' buffers) — DTCM is bytes from full, so no new
 * resident buffers. */
/* SD-media picker value buffers (photo speed / GIF file / alarm sound) are NOT
 * resident: they live on clock_settings_menu()'s stack for the life of the
 * blocking dialog only, so the tight launcher DTCM carries none of them. */
#define PICK_VAL 40

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
    if (e == ODROID_DIALOG_PREV) s_face_override = (s_face_override <= -1) ? FACE_LAST : s_face_override-1;
    if (e == ODROID_DIALOG_NEXT) s_face_override = (s_face_override >= FACE_LAST) ? -1 : s_face_override+1;
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
static bool cb_autodim(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) s_autodim = !s_autodim;
  sprintf(o->value, "%s", s_autodim ? curr_lang->s_Clock_On : curr_lang->s_Clock_Off);
  return e == ODROID_DIALOG_ENTER; }
/* Night full-off window: start-hour row. Cycles Off -> 21:00 -> 22:00 ->
 * 23:00 (default) -> 00:00 -> 01:00. "Off" disables ONLY the night full-off
 * behaviour — day half-dim still follows the Auto-dim row above. The end-hour
 * row right after this one (opts[] index +1, like cb_anim's neighbour rows)
 * is only enabled while a start is chosen, so the two rows can freely combine
 * instead of cycling a fixed preset list. */
static bool cb_night_start(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    int idx = (s_night_start == NIGHT_OFF) ? -1 : night_start_index(s_night_start);
    if (idx < -1) idx = -1;   /* defensive: a corrupt value behaves like Off */
    if (e == ODROID_DIALOG_PREV) idx = (idx <= -1) ? NIGHT_START_COUNT - 1 : idx - 1;
    if (e == ODROID_DIALOG_NEXT) idx = (idx >= NIGHT_START_COUNT - 1) ? -1 : idx + 1;
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT)
        s_night_start = (idx < 0) ? NIGHT_OFF : NIGHT_START_HOURS[idx];
    o[1].enabled = (s_night_start == NIGHT_OFF) ? -1 : 1;   /* the end-hour row right after this one */
    if (s_night_start == NIGHT_OFF) sprintf(o->value, "%s", curr_lang->s_Clock_Off);
    else sprintf(o->value, "%02d:00", s_night_start);
    return e == ODROID_DIALOG_ENTER;
}
/* Night full-off window: end-hour row, cycling 05:00..09:00. Only meaningful
 * (and only enabled — see cb_night_start above) once Start != Off. */
static bool cb_night_end(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    int idx = night_end_index(s_night_end);
    if (idx < 0) idx = 0;   /* defensive: a corrupt value snaps to the first entry */
    if (e == ODROID_DIALOG_PREV) idx = (idx == 0) ? NIGHT_END_COUNT - 1 : idx - 1;
    if (e == ODROID_DIALOG_NEXT) idx = (idx + 1) % NIGHT_END_COUNT;
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) s_night_end = NIGHT_END_HOURS[idx];
    sprintf(o->value, "%02d:00", s_night_end);
    return e == ODROID_DIALOG_ENTER;
}
/* System backlight brightness — identical to the common PAUSE menu's row
 * (odroid_overlay.c brightness_update_cb): same odroid_display_get_backlight()
 * / odroid_display_set_backlight() calls, so a change here persists via
 * odroid_settings and is reflected everywhere else too, and the same
 * fill/blank gauge rendering. The clock previously had its own dedicated
 * brightness concept (follow-system vs. pinned); device feedback showed it
 * only confused users, so this row now just IS the system brightness. */
static bool cb_bright(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    int8_t level = odroid_display_get_backlight();
    int8_t max = ODROID_BACKLIGHT_LEVEL_COUNT - 1;

    if (e == ODROID_DIALOG_PREV && level > 0)   odroid_display_set_backlight(--level);
    if (e == ODROID_DIALOG_NEXT && level < max) odroid_display_set_backlight(++level);

    char a = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Fill[0] : curr_lang->s_Full[0];
    char b = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Full[0] : curr_lang->s_Fill[0];
    for (int i = 0; i <= max; i++) o->value[i] = (i <= level) ? a : b;
    o->value[max + 1] = 0;
    return e == ODROID_DIALOG_ENTER;
}
/* Next selectable background value in `dir` (+1/-1). Ambient(1) is retired on
 * every build; GIF/photo are compiled out on flash builds — so a card-less unit
 * cycles only Off <-> Scene. */
static int anim_step(int cur, int dir)
{
    for (;;) {
        cur = (cur + dir + ANIM_COUNT) % ANIM_COUNT;
        if (cur == 1) continue;                                 /* ambient retired */
#if !CLOCK_SD_MEDIA
        if (cur == ANIM_GIF || cur == ANIM_PHOTO) continue;     /* SD-only media */
#endif
        return cur;
    }
}
static bool cb_anim(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_anim = anim_step(s_anim, -1);
    if (e == ODROID_DIALOG_NEXT) s_anim = anim_step(s_anim, +1);
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) {
#if CLOCK_SD_MEDIA
        clock_gif_set_file(s_bgfile);
        if (s_anim == ANIM_GIF) { if (clock_gif_load()) s_album_used = true; } else clock_gif_free();
        if (s_anim == ANIM_PHOTO) { if (!clock_album_ready()) clock_album_open(); s_album_used = true; s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS; s_fade_start = 0; }
#endif
        /* opts[] order after this row is: [+1] Scene, [+2] Photo speed, [+3] GIF
         * file. Each is live only for its own background. */
        o[1].enabled = (s_anim == ANIM_SCENE) ? 1 : -1;
#if CLOCK_SD_MEDIA
        o[2].enabled = (s_anim == ANIM_PHOTO) ? 1 : -1;
        o[3].enabled = (s_anim == ANIM_GIF)   ? 1 : -1;
#endif
    }
    const char *lv = (s_anim == 0) ? curr_lang->s_Clock_Anim_0
                   : (s_anim == 1) ? curr_lang->s_Clock_Anim_1
                   : (s_anim == ANIM_SCENE) ? curr_lang->s_Clock_Anim_2
                   : (s_anim == ANIM_GIF) ? curr_lang->s_Clock_Anim_3
                   : curr_lang->s_Clock_Anim_4;
    const char *why = "";
#if CLOCK_SD_MEDIA
    if (s_anim == ANIM_GIF && !clock_gif_ready()) {
        int st = clock_gif_status();
        why = (st == CLOCK_GIF_NO_RAM)  ? " (no RAM)"
            : (st == CLOCK_GIF_BAD_DIMS) ? " (too big)"
            : (st == CLOCK_GIF_BAD_FMT)  ? " (bad gif)"
            : " (no file)";
    }
    /* mirror the GIF diagnostic: an empty /clock/album must say so instead of
     * silently keeping the plain theme background (bit us on flash builds) */
    if (s_anim == ANIM_PHOTO && !clock_album_ready())
        why = " (no photos)";
#endif
    snprintf(o->value, sizeof v_anim, "%s%s", lv, why);
    return e == ODROID_DIALOG_ENTER;
}
static bool cb_scene(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_scene = (s_scene == 0) ? SCENE_COUNT - 1 : s_scene - 1;
    if (e == ODROID_DIALOG_NEXT) s_scene = (s_scene + 1) % SCENE_COUNT;
    if (s_scene < 0 || s_scene >= SCENE_COUNT) s_scene = 0;
    snprintf(o->value, sizeof v_scene, "%s", SCENE_NAMES[s_scene]);
    return e == ODROID_DIALOG_ENTER;
}
#if CLOCK_SD_MEDIA
static bool cb_pspeed(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    if (e == ODROID_DIALOG_PREV) s_photo_speed = (s_photo_speed == 0) ? 2 : s_photo_speed - 1;
    if (e == ODROID_DIALOG_NEXT) s_photo_speed = (s_photo_speed + 1) % 3;
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) { s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS; s_fade_start = 0; }
    snprintf(o->value, PICK_VAL, "%s",
             (s_photo_speed == 0) ? "Slow" : (s_photo_speed == 2) ? "Fast" : "Normal");
    return e == ODROID_DIALOG_ENTER;
}

/* ---- menu-only file pickers (GIF background file / alarm sound) ----------
 * Rescan /clock on every menu use so no file list is held resident — only the
 * chosen basename lives in cfg. A name too long for the bounded buffer is
 * skipped from the picker (never truncated, never a crash). */
#define PICK_MAX  16
/* n[] width matches s_bgfile/s_alarmsnd so any name the picker lists also fits
 * the persistent store — pick_scan_cb skips names too long for this buffer. */
typedef struct { char n[PICK_MAX][32]; int count; const char *e1, *e2; } filelist_t;

/* case-insensitive extension match on a basename */
static bool name_has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot + 1, ext) == 0;
}
static int pick_scan_cb(const rg_scandir_t *f, void *arg)
{
    filelist_t *L = arg;
    if (!f->is_file || f->basename[0] == '.' || L->count >= PICK_MAX) return RG_SCANDIR_CONTINUE;
    if (!name_has_ext(f->basename, L->e1) && (!L->e2 || !name_has_ext(f->basename, L->e2)))
        return RG_SCANDIR_CONTINUE;
    if (strlen(f->basename) >= sizeof L->n[0]) return RG_SCANDIR_CONTINUE;   /* too long -> skip */
    snprintf(L->n[L->count++], sizeof L->n[0], "%s", f->basename);
    return RG_SCANDIR_CONTINUE;
}
static void pick_scan(filelist_t *L, const char *e1, const char *e2)
{
    L->count = 0; L->e1 = e1; L->e2 = e2;
    rg_storage_scandir("/clock", pick_scan_cb, L, 0);
}

/* One shared file-picker row (DRY across the GIF-file and alarm-sound rows).
 *   specials/nspecial : fixed leading slots present on every scan (the synth
 *                       preset labels for the alarm-sound row); NULL/0 = files only
 *   dflt    : what an empty `store` resolves to (bg.gif / alarm.mp3)
 *   store   : the persistent chosen basename (updated on L/R); a special forces it
 * Writes the resolved display name into o->value and returns the new selection in
 * *store. An empty folder with no special slots shows "(none)". */
static void pick_row(odroid_dialog_choice_t *o, odroid_dialog_event_t e,
                     const char *e1, const char *e2,
                     const char *const *specials, int nspecial,
                     const char *dflt, char *store, size_t storesz)
{
    filelist_t L; pick_scan(&L, e1, e2);
    int base = nspecial, total = L.count + base;
    if (total == 0) { snprintf(o->value, PICK_VAL, "(none)"); return; }
    int idx = 0;
    bool matched = false;
    for (int i = 0; i < nspecial; i++)
        if (strcasecmp(store, specials[i]) == 0) { idx = i; matched = true; break; }
    if (!matched) {
        const char *want = store[0] ? store : dflt;
        for (int i = 0; i < L.count; i++)
            if (strcasecmp(want, L.n[i]) == 0) { idx = i + base; break; }
    }
    if (e == ODROID_DIALOG_PREV) idx = (idx == 0) ? total - 1 : idx - 1;
    if (e == ODROID_DIALOG_NEXT) idx = (idx + 1) % total;
    const char *sel = (idx < base) ? specials[idx] : L.n[idx - base];
    if (e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT)
        snprintf(store, storesz, "%s", sel);
    snprintf(o->value, PICK_VAL, "%s", sel);
}

/* GIF background file: cycles the found /clock/*.gif. "" (default) resolves to
 * bg.gif if present, else the first file; an empty folder shows "(none)". */
static bool cb_bgfile(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    pick_row(o, e, "gif", NULL, NULL, 0, "bg.gif", s_bgfile, sizeof s_bgfile);
    if ((e == ODROID_DIALOG_PREV || e == ODROID_DIALOG_NEXT) && s_anim == ANIM_GIF) {
        clock_gif_set_file(s_bgfile);
        if (clock_gif_load()) s_album_used = true;
    }
    return e == ODROID_DIALOG_ENTER;
}

/* Alarm sound: the four synth presets (leading slots) then each /clock/*.mp3
 * |*.wav. "" resolves to alarm.mp3 (back-compat) if present, else the presets;
 * a preset label forces the synth (and updates s_beep_preset). */
static bool cb_alarmsnd(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{
    (void)r;
    pick_row(o, e, "mp3", "wav", BEEP_LABELS, RG_TONE_COUNT, "alarm.mp3", s_alarmsnd, sizeof s_alarmsnd);
    int p = rg_tone_preset_from_token(s_alarmsnd);   /* a preset chosen -> keep it as the synth shape */
    if (p >= 0) s_beep_preset = p;
    return e == ODROID_DIALOG_ENTER;
}
#endif /* CLOCK_SD_MEDIA */
static bool cb_vol(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{   /* edits the clock's OWN alarm loudness (s_alarm_volume) — NOT the system
     * volume, so a quiet gaming volume can't silently mute the morning alarm.
     * Same 0..9 scale, curve AND bar-gauge look as the common volume row
     * (odroid_overlay.c volume_update_cb); 0 = a silent alarm (the ring's
     * digit-pulse overlay is still the visual alert). The system volume is
     * untouched here — users adjust it via the common PAUSE menu. */
    int lv = s_alarm_volume;
    if (e == ODROID_DIALOG_PREV && lv > 0) s_alarm_volume = --lv;
    if (e == ODROID_DIALOG_NEXT && lv < ODROID_AUDIO_VOLUME_MAX) s_alarm_volume = ++lv;
    char a = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Fill[0] : curr_lang->s_Full[0];
    char b = (e == ODROID_DIALOG_INIT && o->id == (int)r) ? curr_lang->s_Full[0] : curr_lang->s_Fill[0];
    for (int i = 0; i <= ODROID_AUDIO_VOLUME_MAX; i++) o->value[i] = (i <= lv) ? a : b;
    o->value[ODROID_AUDIO_VOLUME_MAX + 1] = 0;
    return e == ODROID_DIALOG_ENTER; }
static bool cb_enter(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; o->value[0] = 0; return e == ODROID_DIALOG_ENTER; }

/* "절전"(power-save) group row: collapses Auto-dim + Night-start + Night-end
 * into a single main-menu entry (those 3 rows used to be separate; device
 * feedback said the main popup had too many rows and started scrolling —
 * see clock_settings_menu). The value mirrors the Auto-dim toggle; A/ENTER
 * opens clock_powersave_menu() below with the full 3 rows. L/R deliberately
 * do nothing here so there is only one place that actually changes Auto-dim
 * (the sub-dialog's own row) — no risk of the two getting out of sync. */
static bool cb_powersave_group(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r)
{ (void)r; sprintf(o->value, "%s", s_autodim ? curr_lang->s_Clock_On : curr_lang->s_Clock_Off);
  return e == ODROID_DIALOG_ENTER; }

static void clock_menu_repaint(void)
{
    uint16_t *fb = lcd_get_active_buffer();
    uint16_t bg = TH()->scr;
    fb_fill_screen(fb, bg);
    if (s_anim == ANIM_SCENE) { draw_scene(HAL_GetTick(), TH()); s_ghost_on = false; }
    else if (s_anim == 1) { draw_ambient(HAL_GetTick(), TH()->ink); s_ghost_on = false; }
#if CLOCK_SD_MEDIA
    else if (s_anim == ANIM_GIF && clock_gif_ready()) { clock_gif_blit(fb, HAL_GetTick()); s_ghost_on = false; }
#endif
    else s_ghost_on = true;
    draw_topbar(MODE_CLOCK, true);   /* the live settings preview always shows the pager */
    render_clock(HAL_GetTick(), false);
}

/* Sub-dialog opened from the main menu's "절전" group row: Auto-dim toggle +
 * the two night-window hour rows. Reuses the SAME callbacks
 * (cb_autodim/cb_night_start/cb_night_end) the main menu used to call
 * directly for these three rows, so the persisted cfg keys and the
 * conditional Night-end enable/disable logic (cb_night_start toggles o[1])
 * are completely unchanged — only WHERE the rows are shown moved. A nested
 * odroid_overlay_dialog call, exactly like the main clock menu dispatching
 * to a second screen for Set Time / Alarms. */
static void clock_powersave_menu(void)
{
    char v_night_start[8], v_night_end[8];
    odroid_dialog_choice_t opts[] = {
        {11, curr_lang->s_Clock_Auto_Dim, v_autodim, 1, cb_autodim},
        {14, curr_lang->s_Clock_Night_Off, v_night_start, 1, cb_night_start},
        {16, curr_lang->s_Clock_Night_End, v_night_end, (s_night_start == NIGHT_OFF) ? -1 : 1, cb_night_end},
        ODROID_DIALOG_CHOICE_LAST
    };
    for (odroid_dialog_choice_t *o = opts; o->update_cb; o++)
        o->update_cb(o, ODROID_DIALOG_FOCUS_GAINED, 0);
    odroid_overlay_dialog(curr_lang->s_Clock_Auto_Dim, opts, 0, &clock_menu_repaint, 0);
}

/* returns true when the user picked "Exit" — the caller leaves the app */
static bool clock_settings_menu(void)
{
    /* Order: the everyday actions first (set time, format, alarms, volume,
     * brightness, power-save — device feedback asked for these grouped
     * together instead of scattered), then the look-and-feel pickers. The
     * choice `id`s are fixed (the post-dialog dispatch keys off them), so
     * rows can be reordered freely. Anim/Scene/Photo-speed/GIF-file MUST stay
     * adjacent in that order — cb_anim toggles the three that follow it via
     * o[1]/o[2]/o[3]. Alarm sound and DND now live inside the Alarms popup
     * (clock_alarm_setup/ALARM_IDX_*) and Auto-dim/Night-start/Night-end now
     * live inside the "절전" group's own sub-dialog (clock_powersave_menu) —
     * both moved off this menu to stay under the no-scroll row budget (see
     * odroid_overlay_draw_dialog's had_extent: header + N*14px rows + 34px
     * padding must stay <= 230px, i.e. at most 14 rows; this menu has 13 with
     * SD media compiled in, 11 without). The SD-media rows (photo speed, GIF
     * file) are compiled out entirely on flash builds. */
    /* stack-resident (DTCM is bytes from full): the brightness gauge, and
     * (SD builds) the photo-speed / GIF-file picker values */
    char v_bright[ODROID_BACKLIGHT_LEVEL_COUNT + 2];
#if CLOCK_SD_MEDIA
    char v_pspeed[PICK_VAL], v_bgfile[PICK_VAL];
#endif
    odroid_dialog_choice_t opts[] = {
        /* device-wide output controls first, like the common settings menu */
        {15, curr_lang->s_Brightness, v_bright, 1, cb_bright},
        {7, curr_lang->s_Clock_Volume, v_vol,    1, cb_vol},
        {8, curr_lang->s_Clock_Set_Time, v_settime, 1, cb_enter},
        {5, curr_lang->s_Clock_Format, v_fmt,    1, cb_fmt},
        {9, curr_lang->s_Clock_Alarms, v_alarms, 1, cb_enter},
        {17, curr_lang->s_Clock_Auto_Dim, v_autodim, 1, cb_powersave_group},
        {0, curr_lang->s_Clock_Theme,  v_theme,  1, cb_theme},
        {1, curr_lang->s_Clock_Face,   v_face,   1, cb_face},
        {2, curr_lang->s_Clock_Anim,   v_anim,   1, cb_anim},
        {3, curr_lang->s_Clock_Scene,  v_scene,  (s_anim == ANIM_SCENE) ? 1 : -1, cb_scene},
#if CLOCK_SD_MEDIA
        {4, curr_lang->s_Clock_Photo_Speed, v_pspeed, (s_anim == ANIM_PHOTO) ? 1 : -1, cb_pspeed},
        {13, curr_lang->s_Clock_Bg_File, v_bgfile, (s_anim == ANIM_GIF) ? 1 : -1, cb_bgfile},
#endif
        {10, curr_lang->s_Clock_Exit,  v_exit,   1, cb_enter},
        ODROID_DIALOG_CHOICE_LAST
    };
    /* Prime every row's value string (index-free so the SD-media rows can drop
     * out cleanly). cb_anim on FOCUS_GAINED does not touch its neighbours, so
     * the Scene/Photo/GIF `enabled` seeds set in the initializer above stand. */
    for (odroid_dialog_choice_t *o = opts; o->update_cb; o++)
        o->update_cb(o, ODROID_DIALOG_FOCUS_GAINED, 0);

    int sel = odroid_overlay_dialog(curr_lang->s_Clock, opts, 0, &clock_menu_repaint, 0);
    if (sel == 8) clock_edit_time();
    if (sel == 9) clock_alarm_setup();
    if (sel == 17) clock_powersave_menu();
    clock_config_save();
    return sel == 10;
}

/* ---- alarm tone (synthesised, no files) -------------------------------
 *
 * Thin wrapper over the shared generator in rg_alarm.c so the clock ring and
 * the all-state (in-game / music / video) ring use ONE tone implementation and
 * the SAME synth presets. s_beep_preset selects the shape; s_alarm_volume the
 * loudness (its OWN volume, not the system volume — so a quiet gaming volume
 * can't mute the morning alarm; 0 = a silent, visual-only alarm). s_tone_on is
 * kept only so ring_audio() below can tell "the ring just started". */
static bool s_tone_on = false;

static void tone_feed(uint32_t now, bool ringing)
{
    s_tone_on = ringing;
    rg_alarm_tone_feed(now, ringing, s_beep_preset, s_alarm_volume);
}

/* ---- alarm audio dispatch (MP3 file if present, else the synth beep) -----
 *
 * At ring start we pick ONCE: if /clock/alarm.mp3 exists it plays looped through
 * the Music-overlay decoder (rg_clock_alarm_mp3.c); otherwise the synthesised
 * beep above. The decoder overlay lives in RAM_EMU, which the GIF/photo
 * background borrows as its decode arena, so an MP3 ring first SUSPENDS that
 * background (freeing the arena) and RESTORES it when the alarm stops — the ring
 * overlay dominates the screen anyway, so the background just goes solid for the
 * ring. Any MP3 failure falls straight back to the beep here: the alarm is never
 * silent. s_album_used is forced on so the launcher's ROM lists (which share the
 * same arena) are rebuilt on exit. */
#if CLOCK_SD_MEDIA
static bool s_ring_mp3 = false;   /* this ring is playing the MP3, not the beep */

static void clock_bg_suspend(void)   /* free the shared_files arena for the overlay */
{
    if (s_anim == ANIM_GIF)   clock_gif_free();
    if (s_anim == ANIM_PHOTO) clock_album_close();
}
static void clock_bg_restore(void)   /* reload the background after the ring */
{
    if (s_anim == ANIM_GIF)   clock_gif_load();
    if (s_anim == ANIM_PHOTO && clock_album_open()) s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS;
}
#endif

static void ring_audio(uint32_t now, bool ringing)
{
#if CLOCK_SD_MEDIA
    if (!ringing) {                     /* dismissed / snoozed / stopped / leaving */
        if (s_ring_mp3) { clock_alarm_mp3_stop(); clock_bg_restore(); s_ring_mp3 = false; }
        tone_feed(now, false);          /* also stops the beep path if it was used */
        return;
    }
    if (!s_ring_mp3 && !s_tone_on) {     /* ring just started — choose the source once */
        /* A synth-preset token (Beep/Beep2/Chirp/Siren) forces the synth beep;
         * anything else is a file name -> the MP3/WAV path ("" -> alarm.mp3). */
        if (rg_tone_preset_from_token(s_alarmsnd) < 0) {
            clock_alarm_mp3_set_file(s_alarmsnd); /* "" -> alarm.mp3 (back-compat) */
            if (clock_alarm_mp3_available()) {
                clock_bg_suspend();
                /* mp3_start() stages the decoder overlay into the shared_files /
                 * RAM_EMU arena (clobbering the launcher's ROM lists) BEFORE it can
                 * still fail (unreadable / undecodable file). Mark the arena dirty
                 * the moment it can be clobbered — not only on decode success — so
                 * the exit path always rebuilds the lists. With a solid background
                 * clock_bg_restore() is a no-op, so without this an mp3_start()
                 * failure would leave the arena corrupt -> bad tabs / hard fault. */
                s_album_used = true;
                if (clock_alarm_mp3_start()) s_ring_mp3 = true;
                else clock_bg_restore();          /* decode failed -> fall back to the beep */
            }
        }
    }
    if (s_ring_mp3) clock_alarm_mp3_service(volume_tbl[s_alarm_volume]);
    else            tone_feed(now, true);
#else
    tone_feed(now, ringing);            /* flash builds: synth beep only (no SD files) */
#endif
}

/* ---- main loop -------------------------------------------------------- */

void rg_clock_show(void)
{
    clock_mode_t mode = MODE_CLOCK;
    odroid_gamepad_state_t k, prev = {0};
    uint32_t alarm_ring_until = 0;
    uint32_t last_input = HAL_GetTick();   /* mode pager + hint auto-hide after idle */
    bool dirty = true;
    clock_backlight_t bl_state = CLOCK_BL_FULL;   /* idle backlight state: full/dim/off */
    uint32_t anim_freeze_tick = 0;   /* HAL tick background animation was frozen at (DIM/OFF power-save) */

    clock_config_load();
    lcd_backlight_set(odroid_display_get_backlight_raw());   /* apply the system brightness right on entry */
    rg_storage_mkdir("/clock");       /* ensure the clock's folders exist on first run */
    s_snooze_tick = 0;
#if CLOCK_SD_MEDIA
    rg_storage_mkdir("/clock/album"); /* the user drops 320x240 raw .565 photos here */
    s_album_used = false;
    if (s_anim == ANIM_GIF) { clock_gif_set_file(s_bgfile);
        if (clock_gif_load()) s_album_used = true; }   /* GIF borrows shared_files -> restore lists on exit */
    if (s_anim == ANIM_PHOTO) {                 /* borrow shared_files, load first photo */
        if (clock_album_open()) { s_album_used = true; s_photo_next = HAL_GetTick() + PHOTO_HOLD_MS; }
        else s_anim = 0;                        /* no photos / no room -> solid fallback */
    }
#endif
    odroid_input_read_gamepad(&prev);   /* swallow the opening button */

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&k);
        uint32_t now = HAL_GetTick();
        if (k.bitmask & ~prev.bitmask) last_input = now;   /* any new press re-shows the pager + hint */
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

        /* Idle backlight: after CLOCK_DIM_IDLE_MS with no input on the clock
         * face, drop the backlight to half the user's brightness (day) or off
         * entirely (the night window, like a real bedside clock); any input
         * (last_input was just refreshed above) or a ringing alarm restores the
         * user's configured brightness at once. Only on the clock face — never
         * over a running timer/pomodoro/stopwatch the user is watching, and
         * never while the alarm rings. lcd_backlight_set() does NOT touch the
         * persisted odroid_settings brightness, so restoring simply reads it
         * back via odroid_display_get_backlight_raw().
         *
         * Charging exception: a desk clock left on permanent power should not
         * dim during the day. CHARGING and FULL (charge-complete, still on the
         * charger) both mean externally powered — bq24072_get_state() only
         * returns either while its power-good pin reads present — so both
         * count here, same as the existing is_charging convention in
         * odroid_input.c / main_zelda3.c. Night full-off still applies while
         * charging (see clock_should_dim's priority order). */
        bq24072_state_t chg_state = bq24072_get_state();
        bool charging = (chg_state == BQ24072_STATE_CHARGING) || (chg_state == BQ24072_STATE_FULL);
        int night_start_min = (s_night_start == NIGHT_OFF) ? -1 : s_night_start * 60;
        int night_end_min   = s_night_end * 60;
        clock_backlight_t want_bl = clock_should_dim(mode, ringing, s_autodim,
                                                      now - last_input, hh * 60 + mm,
                                                      night_start_min, night_end_min, charging);
        if (want_bl != bl_state) {
            uint8_t user_raw = odroid_display_get_backlight_raw();
            lcd_backlight_set(want_bl == CLOCK_BL_OFF ? 0
                             : want_bl == CLOCK_BL_DIM ? clock_dim_level(user_raw)
                             : user_raw);
            if (bl_state == CLOCK_BL_FULL) anim_freeze_tick = now;   /* just started pausing: freeze the animation clock here */
            if (bl_state == CLOCK_BL_OFF)  dirty = true;             /* leaving OFF: force a repaint (frames were skipped while off) */
            bl_state = want_bl;
        }
        /* DIM and OFF both pause background animation (GIF/scene/photo) to save
         * power — everything that draws from "now" instead reads this frozen
         * tick, so it neither advances nor jumps once brightness is restored.
         * The digit/time face itself keeps using the real "now" below, so it
         * still updates while merely DIMmed. */
        bool bl_paused = bl_state != CLOCK_BL_FULL;
        uint32_t anim_now = bl_paused ? anim_freeze_tick : now;

        if (ringing && (k.bitmask & ~prev.bitmask)) {
            alarm_ring_until = 0;
            ring_audio(now, false);
            if (pressed(&k, &prev, ODROID_INPUT_A))
                s_snooze_tick = now + SNOOZE_MS;       /* A = snooze */
            else
                s_snooze_tick = 0;                     /* anything else = off */
            prev = k; dirty = true;
            HAL_Delay(40);
            continue;
        }
        ring_audio(now, ringing);   /* MP3 file if present, else the synthesised beep */

        /* POWER = SLEEP that RESUMES back into the clock — a bedside clock
         * should not quit on sleep. odroid_system_sleep() fades the CURRENT
         * (clock) frame out (no logo, no launcher flash), STOP-sleeps, and
         * returns in place on wake — the same call the launcher itself makes
         * on POWER. The GIF's open fd is invalidated by the SD unmount/remount
         * across sleep, so drop and reload it. Exit is the PAUSE-menu "Exit"
         * item only. */
        if (pressed(&k, &prev, ODROID_INPUT_POWER)) {
            ring_audio(now, false);
#if CLOCK_SD_MEDIA
            bool had_gif = (s_anim == ANIM_GIF);
            if (had_gif) clock_gif_free();
#endif
            odroid_system_sleep();          /* fade -> STOP sleep -> resume here */
#if CLOCK_SD_MEDIA
            if (had_gif) clock_gif_load();   /* reopen: the pre-sleep fd is stale */
#endif
            /* swallow the wake press so we neither re-sleep nor leak it out */
            do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); }
            while (k.values[ODROID_INPUT_POWER]);
            odroid_input_read_gamepad(&prev);
            s_last_fired_min = -1;   /* re-arm alarms after the sleep gap */
            bl_state = CLOCK_BL_FULL;   /* the sleep resume already restored full brightness... */
            lcd_backlight_set(odroid_display_get_backlight_raw());   /* ...make sure it's the system's configured level */
            last_input = HAL_GetTick();   /* restart the idle timer on wake */
            dirty = true;
            continue;
        }

        /* GAME (START) = quit straight back to the launcher home — the one-press
         * exit that pairs with TIME opening the clock. (During a ring the alarm
         * dismiss below consumes the press first.) Drain the press before leaving
         * so the launcher doesn't read it as a fresh START and pop its About menu
         * — from the clock, GAME just navigates home, nothing else. */
        if (!ringing && pressed(&k, &prev, ODROID_INPUT_START)) {
            do { wdog_refresh(); HAL_Delay(20); odroid_input_read_gamepad(&k); }
            while (k.values[ODROID_INPUT_START]);
            break;
        }

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
            last_input = HAL_GetTick();   /* the blocking menu counts as activity — don't dim on return */
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
        /* the end-of-countdown flash belongs to the timer/pomodoro only —
         * scoping it here stops a rollover from full-screen-flashing (and
         * wiping the GIF/ambient background of) the clock face if the user
         * switches modes inside the 800ms flash window */
        bool flash = (mode == MODE_TIMER || mode == MODE_POMODORO)
                     && s_flash_until > now && ((now/150) & 1);
        if (flash) sig ^= 0x55555555;
        if (ringing) sig ^= (now / 40);    /* ~25fps: smooth 8-bit ring pulse + digit flash */
        /* the chosen background travels with EVERY mode (theme/face/bg stay
         * one consistent set), so its repaint rate applies everywhere too.
         * A GIF that isn't loaded (missing / too big) must NOT keep bumping
         * the signature — else a static face repaints at 12fps for nothing,
         * defeating the event-driven loop and draining the battery. Reads
         * anim_now (frozen while DIM/OFF), so a paused background contributes
         * a constant term here instead of continuing to animate. */
#if CLOCK_SD_MEDIA
        if (s_anim == ANIM_GIF)        { if (clock_gif_ready()) sig ^= (anim_now / 80); }
        else if (s_anim == ANIM_SCENE) sig ^= (anim_now / 32);   /* ~31fps, and 32ms = exactly 2 polls (16ms) so frames land evenly — no beat/jitter */
        else if (s_anim > 0)           sig ^= (anim_now / 320);  /* ambient ~3fps */
#else
        if (s_anim == ANIM_SCENE)      sig ^= (anim_now / 32);   /* pixel scene ~31fps */
        else if (s_anim > 0)           sig ^= (anim_now / 320);  /* ambient ~3fps */
#endif

        /* mode pager + hint fade out after a few idle seconds (any key restores
         * them) — fold into the signature so the hide itself triggers a repaint */
        bool ui_show = (now - last_input) < CLOCK_UI_HIDE_MS;
        if (ui_show) sig ^= 0x02000000u;

        /* photo album auto-advance: hold PHOTO_HOLD_MS, then dip to black, swap
         * to the next photo at the midpoint, and dip back — no hard cut.
         * Skipped entirely while paused (bl_paused) so the slideshow does not
         * advance (and does not silently burn through the fade) while dim/off. */
#if CLOCK_SD_MEDIA
        if (!bl_paused && s_anim == ANIM_PHOTO && clock_album_ready() && clock_album_count() > 1) {
            if (!s_fade_start && s_photo_next && now >= s_photo_next) { s_fade_start = now ? now : 1; s_fade_swapped = false; }
            if (s_fade_start) {
                uint32_t el = now - s_fade_start;
                if (!s_fade_swapped && el >= PHOTO_FADE_MS/2) { clock_album_advance(); s_fade_swapped = true; }
                if (el >= PHOTO_FADE_MS) { s_fade_start = 0; s_photo_next = now + PHOTO_HOLD_MS; }
                sig ^= (now / 33);   /* ~30fps repaint through the dip */
            }
        }
#endif

        /* OFF: the backlight is literally 0, so skip render+flush altogether —
         * the loop above (alarm check, input poll) keeps running at full
         * responsiveness, only the (invisible) paint work is saved. */
        if (bl_state != CLOCK_BL_OFF && (dirty || sig != last_sig)) {
            last_sig = sig; dirty = false;
            uint16_t *fb = lcd_get_active_buffer();
            uint16_t bg = flash ? TH()->ink : TH()->scr;
            fb_fill_screen(fb, bg);
            bool bg_live = false;
            if (!flash) {   /* background layer, identical in every mode; anim_now is
                             * frozen while paused so a DIMmed background holds its
                             * last frame instead of continuing to animate */
#if CLOCK_SD_MEDIA
                if (s_anim == ANIM_GIF && clock_gif_ready()) { clock_gif_blit(fb, anim_now); bg_live = true; }
                else if (s_anim == ANIM_PHOTO && clock_album_ready()) {
                    memcpy(fb, clock_album_current(), (size_t)GW_LCD_WIDTH * GW_LCD_HEIGHT * 2);
                    int fd = photo_fade_darkness(anim_now);   /* dip to black across a photo swap */
                    if (fd) for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = mix565(fb[i], CLOCK_BLACK, fd);
                    bg_live = true;
                }
                else if (s_anim == ANIM_SCENE) { draw_scene(anim_now, TH()); bg_live = true; }
                else if (s_anim == 1) { draw_ambient(anim_now, TH()->ink); bg_live = true; }
#else
                if (s_anim == ANIM_SCENE) { draw_scene(anim_now, TH()); bg_live = true; }
                else if (s_anim == 1) { draw_ambient(anim_now, TH()->ink); bg_live = true; }
#endif
            }
            s_ghost_on = !bg_live;   /* ghost only on a solid theme, not over art */
            switch (mode) {
            case MODE_CLOCK:     render_clock(now, ringing); break;
            case MODE_POMODORO:  render_pomodoro(now);  break;
            case MODE_TIMER:     render_timer(now);     break;
            case MODE_STOPWATCH: render_stopwatch(now); break;
            default: break;
            }
            /* a ringing alarm forces a full-strength pulse over the composed
             * background + face, so it stays legible on GIF/photo/busy scenes */
            if (ringing) draw_ring_overlay(fb, now, TH());
            draw_topbar(mode, ui_show);   /* over the background layers; pager auto-hides */
            /* the hint bar rides the same idle timer as the pager, but a ringing
             * alarm always shows its snooze/stop legend */
            if (ui_show || ringing)
            draw_hintbar(ringing ? curr_lang->s_Clock_Hint_Ring
                : mode == MODE_CLOCK     ? curr_lang->s_Clock_Hint_Clock
                : mode == MODE_POMODORO  ? (s_pomo.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                        : curr_lang->s_Clock_Hint_TimerStop)
                : mode == MODE_TIMER     ? (s_timer.state == RUN_RUNNING ? curr_lang->s_Clock_Hint_Run
                                                                         : curr_lang->s_Clock_Hint_TimerStop)
                : curr_lang->s_Clock_Hint_Run);
            /* the digit pulse only exists on the clock face — give the other
             * modes a visible (and vol=0-proof) ring signal too */
            if (ringing && mode != MODE_CLOCK && ((now / 200) & 1)) {
                int bw = i18n_get_text_width(curr_lang->s_Clock_Ringing);
                int bx = (GW_LCD_WIDTH - bw) / 2;
                draw_icon(&PIX_BELL, bx - 18, 43, TH()->alarm);
                draw_icon(&PIX_BELL, bx + bw + 7, 43, TH()->alarm);
                draw_centered_i18n(42, curr_lang->s_Clock_Ringing, TH()->alarm);
            }
            lcd_swap();
            lcd_sleep_while_swap_pending();
        }

        prev = k;
        /* Poll fast enough for the active animation so it doesn't stutter: an
         * animated pixel scene runs ~30fps (a 40ms idle poll capped it at 25 and
         * beat against the 33ms frame clock — the "버버벅"). Static faces idle
         * longer to save power. Ringing feeds audio so it polls fastest.
         * Backlight OFF: nothing is animating or being rendered, so idle even
         * longer — still well under the 100ms an input press needs to feel
         * instant, and the alarm check above still runs every iteration. */
        uint32_t poll = ringing ? 8
                      : bl_state == CLOCK_BL_OFF ? 80
#if CLOCK_SD_MEDIA
                      : (s_fade_start || s_anim == ANIM_SCENE) ? 16
                      : (s_anim == ANIM_GIF && clock_gif_ready()) ? 24
#else
                      : (s_anim == ANIM_SCENE) ? 16
#endif
                      : 40;
        HAL_Delay(poll);
    }

    ring_audio(0, false);   /* make sure the SAI is stopped (beep or MP3) on the way out */
    /* Always restore the SYSTEM raw level on the way out — unconditionally,
     * not just when leaving DIM/OFF: idle-dim can have left the physical
     * backlight below the configured level. The launcher (and anything it
     * launches next) must come back at the system's configured brightness. */
    lcd_backlight_set(odroid_display_get_backlight_raw());
#if CLOCK_SD_MEDIA
    clock_gif_free();      /* release the transient GIF cache */
    /* If we borrowed shared_files for photos, its contents are now overwritten.
     * Rebuild the launcher's ROM lists: invalidate every tab, then re-scan the
     * current one so the list is whole on return (faster than one tab switch). */
    if (s_album_used) {
        clock_album_close();
        rg_emulators_reset_all_lists();
        tab_t *cur = gui_get_current_tab();
        if (cur) gui_refresh_tab(cur);
        s_album_used = false;
    }
#endif
    clock_config_save();
}
