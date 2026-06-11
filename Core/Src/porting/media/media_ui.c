// Drawing layer for the Music app — see media_ui.h.
//
// The now-playing screen is a Winamp-style "deck": a beveled title bar, a dark
// LCD glass panel holding a 7-segment elapsed-time readout, the bit-rate / kHz,
// a scrolling track-title marquee and a live spectrum analyzer (Goertzel over
// the audio PCM), then a position slider and a volume / status row, with the
// always-on button-hint bar at the foot. The STATIC layer (title bar + panels +
// hints) is drawn once per track into both framebuffers; the DYNAMIC layer (the
// LCD, analyzer, slider, volume) is repainted every frame into the active one.

#include "media_ui.h"
#include "media_cover.h"
#include "media_audio.h"
#include "gw_lcd.h"
#include "gui.h"
#include "rg_i18n.h"
#include "rg_rtc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SCR_W  GW_LCD_WIDTH
#define SCR_H  GW_LCD_HEIGHT
#define FONT_H 12

// now-playing: deck layout (320x240). Top bar matches the browser header; time,
// bit-rate, volume and status all live together in the info/LCD panel.
#define TITLEBAR_H 18                      // == LIST_HEADER_H (consistent top bar)
#define LCD_Y      24                      // info/LCD panel, dropped off the top bar
#define LCD_H      162                     // 24 .. 186
#define COVER_X    8                       // album-art thumbnail, top-left of panel
#define COVER_Y    30
#define COVER_SZ   56                      // 30 .. 86
#define SEG_X      74                      // 7-segment elapsed time, right of cover
#define SEG_Y      34
#define SEG_W      14                      // per-digit cell
#define SEG_H      28                      // 34 .. 62
#define INFO_X     166                     // bit-rate / kHz column
#define STAT_Y     70                      // play/pause + volume + status + total time
#define MARQUEE_Y  92                      // scrolling title band (below cover)
#define VIS_X      8                       // spectrum analyzer left margin
#define VIS_TOP    108
#define VIS_BASE   182                     // taller equalizer: 108 .. 182
#define VIS_BARS   20
#define SEEK_X     14
#define SEEK_Y     194                     // thin position bar, lowered
#define HINT_DIV   210                     // top of the bottom hint panel
#define HINT1_Y    216

// --- primitives -------------------------------------------------------------

void ui_fill(int x, int y, int w, int h, uint16_t c)
{
    uint16_t *fb = lcd_get_active_buffer();
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= SCR_H) continue;
        uint16_t *row = fb + yy * SCR_W;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx >= 0 && xx < SCR_W) row[xx] = c;
        }
    }
}

static void ui_px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        uint16_t *fb = lcd_get_active_buffer();
        fb[y * SCR_W + x] = c;
    }
}

// Paint the four corner cut-outs of a rect with `c`, giving a radius-`r` round.
static void round_corners(int x, int y, int w, int h, int r, uint16_t c)
{
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int dx = r - i, dy = r - j;
            if (dx * dx + dy * dy > r * r) {
                ui_px(x + i, y + j, c);
                ui_px(x + w - 1 - i, y + j, c);
                ui_px(x + i, y + h - 1 - j, c);
                ui_px(x + w - 1 - i, y + h - 1 - j, c);
            }
        }
}

// A rounded outline (used for the cover frame).
static void ui_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    ui_fill(x + r, y, w - 2 * r, 1, c);
    ui_fill(x + r, y + h - 1, w - 2 * r, 1, c);
    ui_fill(x, y + r, 1, h - 2 * r, c);
    ui_fill(x + w - 1, y + r, 1, h - 2 * r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int dx = r - i, dy = r - j, d = dx * dx + dy * dy;
            if (d <= r * r && d > (r - 1) * (r - 1)) {
                ui_px(x + i, y + j, c);
                ui_px(x + w - 1 - i, y + j, c);
                ui_px(x + i, y + h - 1 - j, c);
                ui_px(x + w - 1 - i, y + h - 1 - j, c);
            }
        }
}

int ui_text(int x, int y, int w, const char *t, uint16_t fg, uint16_t bg)
{
    return i18n_draw_text_line(x, y, w, t, fg, bg, 0);
}

int ui_text_t(int x, int y, int w, const char *t, uint16_t fg)
{
    return i18n_draw_text_line(x, y, w, t, fg, 0, 1);
}

void ui_text_center(int y, const char *t, uint16_t fg)
{
    int w = i18n_get_text_width(t);
    ui_text_t((SCR_W - w) / 2, y, w + 2, t, fg);
}

void ui_text_center_t(int y, const char *t, uint16_t fg) { ui_text_center(y, t, fg); }

void ui_text_bold_center_t(int y, const char *t, uint16_t fg)
{
    int w = i18n_get_text_width(t);
    int x = (SCR_W - w) / 2;
    ui_text_t(x, y, w + 3, t, fg);
    ui_text_t(x + 1, y, w + 3, t, fg);    // faux-bold: second pass offset 1px
}

uint16_t ui_dim(uint16_t c, int num, int den)
{
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = r * num / den; g = g * num / den; b = b * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Blend a toward b by t/16.
static uint16_t ui_mix(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (br - ar) * t / 16;
    int g = ag + (bg - ag) * t / 16;
    int bl = ab + (bb - ab) * t / 16;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// A lifted, lightly accent-tinted control surface relative to the theme — so the
// title/control bars read as distinct panels instead of melting into a dark bg.
static uint16_t ui_player_surface(void)
{
    uint16_t base = ui_mix(curr_colors->bg_c, curr_colors->main_c, 2);
    return ui_mix(base, curr_colors->sel_c, 1);
}

// Vertical gradient background (subtle, themed).
static void draw_vbg(void)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t top = ui_mix(bg, curr_colors->sel_c, 2);   // faint accent tint up top
    uint16_t *fb = lcd_get_active_buffer();
    for (int y = 0; y < SCR_H; y++) {
        uint16_t c = ui_mix(top, bg, y * 16 / SCR_H);
        uint16_t *row = fb + y * SCR_W;
        for (int x = 0; x < SCR_W; x++) row[x] = c;
    }
}

// --- small geometric icons --------------------------------------------------

static void icon_play(int x, int y, int w, int h, uint16_t c)
{
    for (int cx = 0; cx < w; cx++) {
        int colh = h * (w - cx) / w;
        if (colh < 1) colh = 1;
        ui_fill(x + cx, y + (h - colh) / 2, 1, colh, c);
    }
}

static void icon_pause(int x, int y, int w, int h, uint16_t c)
{
    int bw = w * 3 / 8;
    ui_fill(x, y, bw, h, c);
    ui_fill(x + w - bw, y, bw, h, c);
}

// small filled circle (knob)
static void dot(int cx, int cy, int r, uint16_t c)
{
    uint16_t *fb = lcd_get_active_buffer();
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && px < SCR_W && py >= 0 && py < SCR_H)
                    fb[py * SCR_W + px] = c;
            }
}

// hardware-like vertical volume pips
static void draw_vol_pips(int x, int y, int vol)
{
    uint16_t accent = curr_colors->sel_c;
    uint16_t bg = curr_colors->bg_c;
    uint16_t dim = ui_mix(curr_colors->dis_c, bg, 10);
    for (int i = 0; i < 9; i++) {
        int h = 2 + i; // height from 2 to 10
        int px = x + i * 3;
        int py = y + 10 - h; // bottom-aligned
        uint16_t c = (i < vol) ? accent : dim;
        ui_fill(px, py, 2, h, c);
    }
}

// language-neutral function icons
// Feather "volume-2": speaker body + cone + two sound arcs.
static void icon_speaker(int x, int y, uint16_t c)
{
    ui_fill(x, y + 3, 3, 4, c);                       // body
    for (int cx = 0; cx < 4; cx++) {                  // cone
        int hh = 2 + cx * 2;
        ui_fill(x + 3 + cx, y + 5 - hh / 2, 1, hh, c);
    }
    ui_px(x + 8, y + 2, c); ui_px(x + 9, y + 3, c);   // inner arc
    ui_px(x + 9, y + 4, c); ui_px(x + 9, y + 5, c);
    ui_px(x + 8, y + 6, c);
}

// Feather "skip-forward"/track: a filled triangle nudged by an end bar.
static void icon_track(int x, int y, uint16_t c)
{
    for (int i = 0; i < 6; i++) {                      // right triangle
        int hh = 9 - i; if (hh < 1) hh = 1;
        ui_fill(x + i, y + (9 - hh) / 2 + 1, 1, hh, c);
    }
    ui_fill(x + 7, y + 1, 2, 9, c);                    // end bar
}

static void icon_menu(int x, int y, uint16_t c)       // hamburger
{
    ui_fill(x, y + 1, 11, 2, c);
    ui_fill(x, y + 5, 11, 2, c);
    ui_fill(x, y + 9, 11, 2, c);
}

// folder icon centered in an sz×sz cell
static void icon_folder(int x, int y, int sz, uint16_t c)
{
    int w = sz - 4, h = sz - 12, ox = x + 2, oy = y + (sz - h) / 2 + 2;
    ui_fill(ox, oy - 3, w / 2, 3, c);                 // tab
    ui_fill(ox, oy, w, h, c);                         // body
    ui_px(ox + w / 2, oy - 3, c);
}

static void icon_shuffle(int x, int y, uint16_t c)
{
    for (int i = 0; i < 8; i++) { ui_px(x + 1 + i, y + 1 + i, c); ui_px(x + 1 + i, y + 8 - i, c); }
    ui_px(x + 7, y, c); ui_px(x + 9, y, c); ui_px(x + 9, y + 2, c);     // arrow tips
    ui_px(x + 7, y + 9, c); ui_px(x + 9, y + 9, c); ui_px(x + 9, y + 7, c);
}

static void icon_repeat(int x, int y, uint16_t c)
{
    ui_fill(x + 1, y + 1, 8, 1, c);                   // top
    ui_fill(x + 8, y + 1, 1, 6, c);                   // right
    ui_fill(x + 2, y + 7, 7, 1, c);                   // bottom
    ui_fill(x + 1, y + 3, 1, 4, c);                   // left
    ui_px(x + 2, y - 1, c); ui_px(x + 3, y, c); ui_px(x + 3, y + 2, c);  // arrowhead
}

// Copy `s` into `out`, truncating with a trailing ".." if it is wider than maxw.
static void ui_ellipsize(char *out, int cap, const char *s, int maxw)
{
    if (!s) { out[0] = '\0'; return; }
    if (i18n_get_text_width(s) <= maxw) {
        snprintf(out, cap, "%s", s);
        return;
    }
    int dotw = i18n_get_text_width("..");
    int budget = maxw - dotw;
    int o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o < cap - 4) {
        int n = (*p < 0x80) ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
        char tmp[8];
        for (int i = 0; i < n; i++) tmp[i] = (char)p[i];
        tmp[n] = '\0';
        out[o] = '\0';
        char probe[256];
        snprintf(probe, sizeof(probe), "%s%s", out, tmp);
        if (i18n_get_text_width(probe) > budget) break;
        for (int i = 0; i < n; i++) out[o++] = (char)p[i];
        p += n;
    }
    out[o] = '\0';
    snprintf(out + o, cap - o, "..");
}

// --- LCD 7-segment time -----------------------------------------------------

// segment bitmask: a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40
static const uint8_t SEG_MAP[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void draw_7seg(int x, int y, int digit, uint16_t on, uint16_t off)
{
    const int t = 3, W = SEG_W, H = SEG_H;
    int vlen = (H - 3 * t) / 2;
    int midY = y + t + vlen;
    uint8_t m = (digit >= 0 && digit <= 9) ? SEG_MAP[digit] : 0;
    ui_fill(x + t,     y,         W - 2 * t, t,    (m & 0x01) ? on : off);  // a
    ui_fill(x + W - t, y + t,     t,         vlen, (m & 0x02) ? on : off);  // b
    ui_fill(x + W - t, midY + t,  t,         vlen, (m & 0x04) ? on : off);  // c
    ui_fill(x + t,     y + H - t, W - 2 * t, t,    (m & 0x08) ? on : off);  // d
    ui_fill(x,         midY + t,  t,         vlen, (m & 0x10) ? on : off);  // e
    ui_fill(x,         y + t,     t,         vlen, (m & 0x20) ? on : off);  // f
    ui_fill(x + t,     midY,      W - 2 * t, t,    (m & 0x40) ? on : off);  // g
}

// Render M:SS (minutes 0..99) as lit/unlit segments; returns the end x.
static int draw_lcd_time(int sec, uint16_t on, uint16_t off)
{
    if (sec < 0) sec = 0;
    int mm = sec / 60, ss = sec % 60;
    if (mm > 99) mm = 99;
    int x = SEG_X;
    if (mm >= 10) { draw_7seg(x, SEG_Y, mm / 10, on, off); x += SEG_W + 4; }
    draw_7seg(x, SEG_Y, mm % 10, on, off); x += SEG_W + 7;
    ui_fill(x, SEG_Y + 7, 3, 3, on); ui_fill(x, SEG_Y + 18, 3, 3, on); x += 3 + 7;  // colon
    draw_7seg(x, SEG_Y, ss / 10, on, off); x += SEG_W + 4;
    draw_7seg(x, SEG_Y, ss % 10, on, off); x += SEG_W;
    return x;
}

// --- spectrum analyzer (Goertzel over captured PCM) -------------------------

#define VIS_FS 48000                       // engine resamples mono to 48 kHz
#define VIS_N  256                          // analysis window (power of two)

static int16_t g_vis[VIS_N];
static int      g_vis_w;                    // ring write index (free-running)
static float    g_bar[VIS_BARS], g_peak[VIS_BARS], g_coeff[VIS_BARS];
static bool     g_vis_ready;

// Capture one mono sample for the analyzer (called from the audio feed loop).
void ui_vis_push(int16_t s) { g_vis[g_vis_w & (VIS_N - 1)] = s; g_vis_w++; }

static void vis_init(void)
{
    for (int i = 0; i < VIS_BARS; i++) {
        float frac = (VIS_BARS > 1) ? (float)i / (VIS_BARS - 1) : 0.0f;
        float f = 60.0f * powf(14000.0f / 60.0f, frac);     // log-spaced 60Hz..14kHz
        g_coeff[i] = 2.0f * cosf(2.0f * 3.14159265f * f / (float)VIS_FS);
        g_bar[i] = g_peak[i] = 0.0f;
    }
    g_vis_ready = true;
}

// One Goertzel pass per band over the latest VIS_N samples (when `active`),
// then bar gravity + slowly-falling peak caps — the classic analyzer feel.
static void vis_compute(bool active)
{
    if (!g_vis_ready) vis_init();
    int w = g_vis_w;
    for (int b = 0; b < VIS_BARS; b++) {
        float v = 0.0f;
        if (active) {
            float coeff = g_coeff[b], s1 = 0.0f, s2 = 0.0f;
            for (int n = 0; n < VIS_N; n++) {
                float x = (float)g_vis[(w - VIS_N + n) & (VIS_N - 1)];
                float s0 = x + coeff * s1 - s2;
                s2 = s1; s1 = s0;
            }
            float mag2 = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            float mag = (mag2 > 0.0f) ? sqrtf(mag2) / (float)VIS_N : 0.0f;
            // log map with headroom so loud content sits near ~0.85 (not pegged)
            // and quiet detail still lifts off the floor.
            v = log10f(1.0f + mag * 0.5f) / log10f(1.0f + 30000.0f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
        }
        if (v > g_bar[b]) g_bar[b] = v; else g_bar[b] -= 0.07f;
        if (g_bar[b] < 0.0f) g_bar[b] = 0.0f;
        if (g_bar[b] > g_peak[b]) g_peak[b] = g_bar[b]; else g_peak[b] -= 0.025f;
        if (g_peak[b] < 0.0f) g_peak[b] = 0.0f;
    }
}

// Retro LED-VU analyzer: each bar is a stack of discrete LED segments (lit ones
// coloured by zone like a hardware meter, unlit ones a faint ghost), with a
// floating peak-hold LED on top.
#define VIS_SEG  4                          // LED segment height
#define VIS_GAP  1
static void draw_spectrum(uint16_t lcd_bg)
{
    const int STEP = VIS_SEG + VIS_GAP;
    int pitch = (SCR_W - 2 * VIS_X) / VIS_BARS;
    int bw = pitch - 4; if (bw < 3) bw = 3;
    int H = VIS_BASE - VIS_TOP;
    int rows = H / STEP;
    uint16_t lo   = curr_colors->sel_c;                     // green zone
    uint16_t mid  = ui_mix(lo, curr_colors->main_c, 9);     // amber zone
    uint16_t hi   = curr_colors->main_c;                    // hot zone
    uint16_t off  = ui_mix(lcd_bg, lo, 1);                  // unlit ghost LED
    uint16_t peak = ui_mix(curr_colors->main_c, lo, 2);

    ui_fill(VIS_X, VIS_TOP - 2, SCR_W - 2 * VIS_X, H + 4, lcd_bg);
    for (int b = 0; b < VIS_BARS; b++) {
        int x = VIS_X + b * pitch + 1;
        int lit = (int)(g_bar[b] * rows + 0.5f);
        int pk  = (int)(g_peak[b] * rows + 0.5f);
        for (int s = 0; s < rows; s++) {
            int yy = VIS_BASE - (s + 1) * STEP + VIS_GAP;
            uint16_t c;
            if (s == pk - 1 && pk > 0)      c = peak;        // peak-hold LED
            else if (s < lit) {
                int z = s * 100 / (rows > 1 ? rows - 1 : 1);
                c = (z < 55) ? lo : (z < 80) ? mid : hi;
            } else                          c = off;
            ui_fill(x, yy, bw, VIS_SEG, c);
        }
    }
}

// --- scrolling title marquee ------------------------------------------------

static char     g_marquee[256];     // "Artist - Title" for the LCD panel
static int      g_marquee_w;        // pixel width (set per track in static layer)
static uint32_t g_anim;             // frame counter (marquee scroll + analyzer)

static int utf8_len(const unsigned char *p)
{
    return (*p < 0x80) ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
}

// Copy as many whole glyphs of `src` as fit within `maxw` pixels into `out`.
// The result is always <= maxw wide, so it can be drawn at a fixed x without
// ever running past the screen edge (the i18n renderer does not clip overlong
// lines, and its x_pos is unsigned — negative/overflowing x corrupts memory).
static void marquee_fit(char *out, int cap, const char *src, int maxw)
{
    int o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o < cap - 5) {
        int n = utf8_len(p);
        char probe[260];
        memcpy(probe, out, o); memcpy(probe + o, p, n); probe[o + n] = '\0';
        if (i18n_get_text_width(probe) > maxw) break;
        for (int i = 0; i < n; i++) out[o++] = (char)p[i];
        p += n;
    }
    out[o] = '\0';
}

// LED-sign marquee: when the title overflows the band, advance one glyph at a
// time through "TITLE    TITLE    " and show the window that fits. Safe by
// construction — text is always pre-fitted and drawn at a fixed x = VIS_X.
static void draw_marquee(uint16_t lcd_bg)
{
    uint16_t lit = ui_mix(curr_colors->sel_c, curr_colors->main_c, 6);
    int region = SCR_W - 2 * VIS_X;
    ui_fill(VIS_X, MARQUEE_Y - 1, region, 14, lcd_bg);

    if (g_marquee_w <= region) {
        int x = VIS_X + (region - g_marquee_w) / 2;
        ui_text(x, MARQUEE_Y, g_marquee_w + 2, g_marquee, lit, lcd_bg);
        return;
    }

    char period[300];
    snprintf(period, sizeof(period), "%s    ", g_marquee);     // 4-space gap
    int glyphs = 0;
    for (const unsigned char *p = (const unsigned char *)period; *p; p += utf8_len(p))
        glyphs++;
    if (glyphs < 1) glyphs = 1;

    int s = (int)((g_anim / 6) % (uint32_t)glyphs);            // ~1 glyph / 6 frames
    char dbl[600];
    snprintf(dbl, sizeof(dbl), "%s%s", period, period);
    const unsigned char *p = (const unsigned char *)dbl;
    for (int i = 0; i < s && *p; i++) p += utf8_len(p);

    char win[256];
    marquee_fit(win, sizeof(win), (const char *)p, region);
    ui_text(VIS_X, MARQUEE_Y, region, win, lit, lcd_bg);
}

// --- button-hint chips ------------------------------------------------------

enum { ICN_NONE = 0, ICN_PLAY, ICN_TRACK, ICN_SPEAKER, ICN_MENU };
typedef struct { const char *key; int icon; } chip_t;

static int chip_icon_w(int icon)
{
    if (icon == ICN_NONE) return 0;
    return 11;
}

static void chip_icon_draw(int icon, int x, int y, uint16_t c)
{
    switch (icon) {
        case ICN_PLAY:    icon_play(x, y + 1, 9, 10, c); break;
        case ICN_TRACK:   icon_track(x, y, c); break;
        case ICN_SPEAKER: icon_speaker(x, y + 1, c); break;
        case ICN_MENU:    icon_menu(x, y, c); break;
        default: break;
    }
}

// Draw a centered row of button hints. Each chip is the button label rendered
// as a little keycap (when kh>0) followed by the function icon, e.g. [Ⓐ]▶.
// kh==0 falls back to plain text (for the short list footer with no room).
static void chip_row_k(int y, const chip_t *h, int n, uint16_t keyc, uint16_t iconc, int kh)
{
    const int GAP  = 9;                 // between chips
    const int PADX = (kh > 0) ? 5 : 0;  // keycap horizontal padding
    uint16_t bg = curr_colors->bg_c, accent = curr_colors->sel_c;
    uint16_t surface = ui_player_surface();
    uint16_t cap_fill = ui_mix(bg, accent, 3);
    uint16_t cap_brd  = ui_mix(accent, bg, 6);

    int total = 0;
    for (int i = 0; i < n; i++) {
        int iw = chip_icon_w(h[i].icon);
        total += i18n_get_text_width(h[i].key) + 2 * PADX + (iw ? 4 + iw : 0) + (i ? GAP : 0);
    }
    int x = (SCR_W - total) / 2;
    if (x < 2) x = 2;

    int cy = y - 2;
    for (int i = 0; i < n; i++) {
        if (i) x += GAP;
        int tw = i18n_get_text_width(h[i].key);
        int kw = tw + 2 * PADX;
        if (kh > 0) {                       // keycap behind the label
            ui_fill(x, cy, kw, kh, cap_fill);
            round_corners(x, cy, kw, kh, 4, surface);
            ui_rrect(x, cy, kw, kh, 4, cap_brd);
        }
        ui_text_t(x + PADX, y, tw + 2, h[i].key, keyc);
        x += kw;
        int iw = chip_icon_w(h[i].icon);
        if (iw) { x += 4; chip_icon_draw(h[i].icon, x, y, iconc); x += iw; }
    }
}

static void chip_row(int y, const chip_t *h, int n, uint16_t keyc, uint16_t iconc, uint16_t sepc)
{
    (void)sepc;
    chip_row_k(y, h, n, keyc, iconc, 0);    // plain text (legacy callers / footer)
}

// --- now-playing: the Winamp deck -------------------------------------------

// Deep, faintly accent-tinted "LCD glass".
static uint16_t ui_lcd_bg(void)
{
    uint16_t base = ui_mix(curr_colors->bg_c, curr_colors->sel_c, 1);
    return ui_dim(base, 2, 5);
}

static void blit_thumb(const uint16_t *art, int sz, int x, int y);

// Small album-art thumbnail for the deck — decoded once per track by the player
// (cheap, list-sized) and handed to us as a pointer into its static buffer.
static const uint16_t *g_deck_cover;
static int             g_deck_cover_sz;
static bool            g_deck_has_cover;

void ui_player_set_cover(const uint16_t *thumb, int sz, bool has)
{
    g_deck_cover = thumb; g_deck_cover_sz = sz; g_deck_has_cover = has;
}

// A little record (concentric grooves + centre label) — the no-cover stand-in.
static void draw_disc(int cx, int cy, int r, uint16_t accent, uint16_t bg)
{
    uint16_t slate  = ui_mix(bg, accent, 2);
    uint16_t groove = ui_mix(bg, accent, 5);
    dot(cx, cy, r,            slate);       // disc body (drawn large -> small)
    dot(cx, cy, r * 86 / 100, groove);
    dot(cx, cy, r * 84 / 100, slate);
    dot(cx, cy, r * 68 / 100, groove);
    dot(cx, cy, r * 66 / 100, slate);
    dot(cx, cy, r * 50 / 100, groove);
    dot(cx, cy, r * 48 / 100, slate);
    dot(cx, cy, r * 34 / 100, accent);      // bright centre label
    int nw = i18n_get_text_width("\xE2\x99\xAA");
    ui_text_t(cx - nw / 2, cy - 6, nw + 2, "\xE2\x99\xAA", bg);   // ♪ on the label
}

// Album art (or the record stand-in), rounded + thin-framed, in the LCD panel.
static void draw_deck_cover(uint16_t lcd_bg)
{
    uint16_t accent = curr_colors->sel_c;
    int x = COVER_X, y = COVER_Y, s = COVER_SZ;
    if (g_deck_has_cover && g_deck_cover && g_deck_cover_sz == s)
        blit_thumb(g_deck_cover, s, x, y);
    else {
        ui_fill(x, y, s, s, ui_mix(lcd_bg, accent, 1));
        draw_disc(x + s / 2, y + s / 2, s / 2 - 2, accent, lcd_bg);
    }
    round_corners(x, y, s, s, 6, lcd_bg);
    ui_rrect(x, y, s, s, 6, ui_mix(accent, lcd_bg, 6));
}

// Battery level [0..100] + charging flag — provided by the firmware (main_media)
// or the host preview, so this rendering layer stays free of hardware headers.
extern int  media_battery_percent(void);
extern int  media_battery_charging(void);

// Small battery icon (body + nub), filled by charge level; red when low.
static void ui_battery(int x, int y)
{
    int pct = media_battery_percent();
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint16_t bg = curr_colors->bg_c, accent = curr_colors->sel_c, main_c = curr_colors->main_c;
    uint16_t frame = ui_mix(main_c, bg, 5);
    uint16_t fillc = media_battery_charging() ? main_c
                   : (pct <= 20 ? (uint16_t)(0x1F << 11) : accent);   // red when low
    const int W = 16, Hh = 9;
    ui_fill(x, y, W, 1, frame); ui_fill(x, y + Hh - 1, W, 1, frame);   // top/bottom
    ui_fill(x, y, 1, Hh, frame); ui_fill(x + W - 1, y, 1, Hh, frame);  // sides
    ui_fill(x + W, y + 3, 2, 3, frame);                                // nub
    int iw = (W - 4) * pct / 100;
    if (iw > 0) ui_fill(x + 2, y + 2, iw, Hh - 4, fillc);              // charge level
}

// Shared top bar — the browser header and the now-playing deck draw the SAME
// bar: accent tab + title (left), then an optional context label (folder count
// / track index), the battery and the clock (right).
static void ui_topbar(const char *title, const char *right_label)
{
    uint16_t bg = curr_colors->bg_c, accent = curr_colors->sel_c;
    uint16_t soft = ui_mix(curr_colors->main_c, bg, 5);
    uint16_t surface = ui_player_surface();
    const int H = TITLEBAR_H;

    ui_fill(0, 0, SCR_W, H, surface);
    ui_fill(0, H - 1, SCR_W, 1, ui_mix(accent, bg, 4));  // single thin underline, no side bar

    int rx = SCR_W - 8;
    char clk[8];
    snprintf(clk, sizeof(clk), "%02d:%02d", GW_GetCurrentHour(), GW_GetCurrentMinute());
    int cw = i18n_get_text_width(clk);
    rx -= cw;      ui_text(rx, 4, cw + 2, clk, accent, surface);
    rx -= 6 + 18;  ui_battery(rx, 4);
    if (right_label && right_label[0]) {
        int lw = i18n_get_text_width(right_label);
        rx -= 8 + lw; ui_text(rx, 4, lw + 2, right_label, soft, surface);
    }
    char buf[160];
    int titlew = rx - 10; if (titlew < 20) titlew = 20;
    ui_ellipsize(buf, sizeof(buf), title ? title : "", titlew);
    ui_text(8, 4, titlew, buf, accent, surface);
}

static void draw_player_hints(void);

// Static layer: drawn once per track into BOTH framebuffers. The album-art
// thumbnail is supplied separately via ui_player_set_cover().
void ui_player_static(const player_state_t *ps)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t accent = curr_colors->sel_c;
    uint16_t surface = ui_player_surface();
    uint16_t lcd = ui_lcd_bg();

    draw_vbg();

    // the top bar is drawn by ui_topbar() in the dynamic layer (identical to the
    // browser header), so it is not painted here.

    // LCD glass panel
    ui_fill(0, LCD_Y, SCR_W, LCD_H, lcd);
    ui_fill(0, LCD_Y, SCR_W, 1, ui_dim(bg, 1, 3));
    ui_fill(0, LCD_Y + LCD_H, SCR_W, 1, ui_mix(accent, bg, 3));

    // album-art thumbnail (static — the dynamic layer never paints over it)
    draw_deck_cover(lcd);

    // bottom control panel (slider + volume row sit on this)
    ui_fill(0, LCD_Y + LCD_H + 1, SCR_W, HINT_DIV - (LCD_Y + LCD_H + 1), surface);

    // per-track marquee string
    const char *t = (ps->title && ps->title[0]) ? ps->title : "(no title)";
    const char *a = ps->artist ? ps->artist : "";
    if (a[0]) snprintf(g_marquee, sizeof(g_marquee), "%s - %s", a, t);
    else      snprintf(g_marquee, sizeof(g_marquee), "%s", t);
    g_marquee_w = i18n_get_text_width(g_marquee);

    // bottom hint bar (static)
    ui_fill(0, HINT_DIV, SCR_W, SCR_H - HINT_DIV, surface);
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(accent, bg, 4));
    draw_player_hints();
}

// The deck animates continuously while playing — there is no separate "spin".
bool ui_player_has_spin(void) { return true; }

// Dynamic layer: repaint the LCD, analyzer, slider and volume row each frame.
void ui_player_dynamic(const player_state_t *ps)
{
    uint16_t bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t accent = curr_colors->sel_c, dim = curr_colors->dis_c;
    uint16_t surface = ui_player_surface();
    uint16_t lcd = ui_lcd_bg();
    uint16_t lit = ui_mix(accent, main_c, 6);          // bright LCD ink
    uint16_t off = ui_mix(accent, lcd, 13);            // unlit ghost segments
    uint16_t soft = ui_mix(main_c, bg, 5);
    bool scrubbing = ps->scrub >= 0.0f;

    g_anim++;

    // ---- top bar: identical to the browser header (MUSIC + idx + battery + clock) ----
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", ps->track_index + 1, ps->track_count);
    ui_topbar(ps->app_name && ps->app_name[0] ? ps->app_name : "Music", pos);

    // ---- 7-seg elapsed time + bit-rate / kHz ----
    float frac = scrubbing ? ps->scrub
               : (ps->total > 0 ? (float)ps->sec / (float)ps->total : 0.0f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int shown = scrubbing ? (int)(frac * ps->total) : ps->sec;
    ui_fill(SEG_X - 2, SEG_Y - 2, INFO_X - SEG_X, SEG_H + 4, lcd);
    draw_lcd_time(shown, scrubbing ? accent : lit, off);

    ui_fill(INFO_X, SEG_Y, SCR_W - INFO_X - 8, SEG_H, lcd);
    char info[40];
    int br = audio_bitrate_kbps();
    snprintf(info, sizeof(info), "%d kbps", br > 0 ? br : 0);
    ui_text(INFO_X, SEG_Y + 1, SCR_W - INFO_X - 8, info, lit, lcd);
    int hz = audio_src_hz(); if (hz <= 0) hz = VIS_FS;
    snprintf(info, sizeof(info), "%d.%dkHz %s", hz / 1000, (hz % 1000) / 100,
             audio_channels() >= 2 ? "stereo" : "mono");
    ui_text(INFO_X, SEG_Y + 16, SCR_W - INFO_X - 8, info, soft, lcd);

    // ---- status line (in the panel): play/pause + volume | heart/shuffle/repeat + total ----
    ui_fill(SEG_X, STAT_Y - 1, SCR_W - SEG_X - 6, 13, lcd);
    if (ps->paused) icon_pause(SEG_X, STAT_Y + 1, 7, 9, accent);
    else            icon_play (SEG_X, STAT_Y + 1, 8, 9, accent);
    draw_vol_pips(SEG_X + 14, STAT_Y + 1, ps->volume);
    int rx = SCR_W - 8;
    char tot[16];
    snprintf(tot, sizeof(tot), "%d:%02d", ps->total / 60, ps->total % 60);
    int tw = i18n_get_text_width(tot);
    rx -= tw; ui_text(rx, STAT_Y, tw + 2, tot, soft, lcd);
    rx -= 16; ui_text(rx, STAT_Y, 14, "\xE2\x99\xA5",
                      ps->favorite ? accent : ui_mix(dim, bg, 3), lcd);  // ♥
    if (ps->repeat != REPEAT_OFF) { rx -= 14; icon_repeat(rx, STAT_Y + 1, ui_mix(accent, bg, 6)); }
    if (ps->shuffle)              { rx -= 14; icon_shuffle(rx, STAT_Y + 1, ui_mix(accent, bg, 6)); }

    // ---- marquee + spectrum (equalizer) ----
    draw_marquee(lcd);
    vis_compute(!ps->paused);
    draw_spectrum(lcd);

    // ---- thin position bar (lowered) ----
    int gw = SCR_W - 2 * SEEK_X;
    int fillw = (int)(frac * gw);
    ui_fill(SEEK_X - 2, SEEK_Y - 3, gw + 4, 8, surface);
    ui_fill(SEEK_X, SEEK_Y, gw, 2, ui_mix(bg, main_c, 5));
    ui_fill(SEEK_X, SEEK_Y, fillw, 2, accent);
    dot(SEEK_X + fillw, SEEK_Y + 1, 3, scrubbing ? main_c : accent);
}

static void draw_player_hints(void)
{
    uint16_t bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t accent = curr_colors->sel_c;
    static const chip_t chips[] = {
        { "A", ICN_PLAY },
        { "\xE2\x97\x80\xE2\x96\xB6", ICN_TRACK },    // ◀▶  track
        { "\xE2\x96\xB2\xE2\x96\xBC", ICN_SPEAKER },  // ▲▼  volume
        { "PAUSE", ICN_MENU },
    };
    chip_row_k(HINT1_Y, chips, 4, ui_mix(main_c, bg, 1), accent, 16);
}

// --- browser list -----------------------------------------------------------

static void blit_thumb(const uint16_t *art, int sz, int x, int y)
{
    uint16_t *fb = lcd_get_active_buffer();
    for (int j = 0; j < sz; j++) {
        int dy = y + j; if (dy < 0 || dy >= SCR_H) continue;
        for (int i = 0; i < sz; i++) {
            int dx = x + i; if (dx < 0 || dx >= SCR_W) continue;
            fb[dy * SCR_W + dx] = art[j * sz + i];
        }
    }
}

void ui_list_draw(const list_view_t *v, void (*item_at)(int i, list_item_t *out))
{
    uint16_t bg = curr_colors->bg_c, fg = curr_colors->main_c;
    uint16_t accent = curr_colors->sel_c, dim = curr_colors->dis_c;
    uint16_t soft = ui_mix(fg, bg, 4);
    uint16_t panel_bg = ui_player_surface();
    const int H = LIST_HEADER_H, RH = v->row_h, TH = 34;
    bool has_bar = v->count > v->visible_rows;
    int right = SCR_W - (has_bar ? 8 : 4);                      // content right edge
    const int RPAD = 10;     // keep text/time clear of the rounded pill border

    ui_fill(0, 0, SCR_W, SCR_H, bg);

    // header: the shared top bar (accent tab + folder/title + position + battery
    // + clock), identical to the now-playing deck.
    char buf[256], pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", v->count ? v->cursor + 1 : 0, v->count);
    ui_topbar(v->header ? v->header : "", pos);

    if (v->count == 0) {
        const char *hint = (v->empty_hint && v->empty_hint[0]) ? v->empty_hint
                                                               : "(empty)";
        ui_text_center_t(H + 28, hint, fg);
        if (v->empty_sub && v->empty_sub[0]) {
            ui_ellipsize(buf, sizeof(buf), v->empty_sub, SCR_W - 16);
            ui_text_center_t(H + 28 + 22, buf, soft);
        }
    }

    for (int r = 0; r < v->visible_rows; r++) {
        int idx = v->scroll + r;
        if (idx >= v->count) break;
        list_item_t it; memset(&it, 0, sizeof(it));
        item_at(idx, &it);

        int y = H + r * RH;
        bool sel = (idx == v->cursor);
        uint16_t pill_bg = ui_mix(bg, accent, 4);
        uint16_t zebra   = ui_mix(bg, fg, 1);                        // faint alternating row tint
        uint16_t rbg = sel ? pill_bg : ((r & 1) ? zebra : bg);
        uint16_t txt = fg;
        uint16_t sub = sel ? ui_mix(fg, bg, 4) : ui_mix(fg, bg, 7);   // clearer artist/duration

        int pill_x = 4;
        int pill_y = y + 4;          // breathing room above/below each row
        int pill_w = right - pill_x;
        int pill_h = RH - 8;
        int pill_r = 6;
        if (sel) {
            ui_fill(pill_x, pill_y, pill_w, pill_h, pill_bg);
            round_corners(pill_x, pill_y, pill_w, pill_h, pill_r, bg);
            ui_rrect(pill_x, pill_y, pill_w, pill_h, pill_r, ui_mix(bg, accent, 10)); // selection border
        } else if (r & 1) {
            ui_fill(pill_x, pill_y, pill_w, pill_h, zebra);                           // faint zebra band
        }
        int tx = 8, ty = y + (RH - TH) / 2;

        if (it.kind == LIST_SPECIAL) {
            ui_text(tx + 2, y + (RH - 12) / 2, right - tx - 2, it.title, sel ? accent : accent, rbg);
            continue;
        }
        if (it.kind == LIST_DIR) {
            icon_folder(tx, ty, TH, sel ? accent : ui_mix(accent, fg, 8));
            ui_text(tx + TH + 8, y + (RH - 12) / 2, right - (tx + TH + 8), it.title, txt, rbg);
            continue;
        }

        // track row: thumb (rounded), title, artist, duration, heart
        if (it.art && it.art_sz > 0) {
            blit_thumb(it.art, it.art_sz, tx, ty);
        } else {
            ui_fill(tx, ty, TH, TH, ui_mix(rbg, sub, 3));
            int nw = i18n_get_text_width("\xE2\x99\xAA");   // ♪
            ui_text_t(tx + (TH - nw) / 2, ty + (TH - 12) / 2, nw + 2, "\xE2\x99\xAA", ui_mix(rbg, txt, 7));
        }
        round_corners(tx, ty, TH, TH, 6, rbg);
        if (it.playing) {                                   // "now playing" badge on the thumb
            ui_fill(tx, ty, 13, 13, ui_mix(bg, accent, 13));
            round_corners(tx, ty, 13, 13, 4, rbg);
            icon_play(tx + 3, ty + 2, 7, 8, bg);
        }

        int dw = it.duration && it.duration[0] ? i18n_get_text_width(it.duration) : 0;
        int textx = tx + TH + 8;
        int textw = right - textx - (dw ? dw + RPAD + 4 : RPAD);

        ui_ellipsize(buf, sizeof(buf), it.title ? it.title : "", textw - (it.fav ? 16 : 0));
        ui_text(textx, y + 5, textw, buf, txt, rbg);
        if (it.subtitle && it.subtitle[0]) {
            ui_ellipsize(buf, sizeof(buf), it.subtitle, textw);
            ui_text(textx, y + 21, textw, buf, sub, rbg);
        }
        if (it.fav)
            ui_text(right - RPAD - dw - 16, y + 5, 14, "\xE2\x99\xA5", sel ? accent : accent, rbg);   // ♥
        if (dw)
            ui_text(right - RPAD - dw, y + 25, dw + 2, it.duration, sub, rbg);
    }

    // scrollbar
    if (has_bar) {
        int top = H, h = SCR_H - H - LIST_FOOTER_H;
        ui_fill(SCR_W - 4, top, 3, h, ui_dim(dim, 1, 2));
        int th = h * v->visible_rows / v->count; if (th < 14) th = 14;
        int ty = top + (h - th) * v->scroll / (v->count - v->visible_rows > 0 ? v->count - v->visible_rows : 1);
        if (ty + th > top + h) ty = top + h - th;
        ui_fill(SCR_W - 4, ty, 3, th, accent);
    }

    // footer hint
    ui_fill(0, SCR_H - LIST_FOOTER_H, SCR_W, LIST_FOOTER_H, panel_bg);
    ui_fill(0, SCR_H - LIST_FOOTER_H, SCR_W, 1, ui_mix(accent, bg, 4));
    static const chip_t fh[] = {
        { "A", ICN_PLAY }, { "\xE2\x96\xB2\xE2\x96\xBC", ICN_NONE },     // ▲▼
        { "PAUSE", ICN_MENU }, { "B", ICN_NONE },                       // PAUSE = menu
    };
    chip_row(SCR_H - LIST_FOOTER_H + 2, fh, 4, ui_mix(fg, bg, 2), accent, 0);
}

// --- info screen ------------------------------------------------------------

static void info_row(int *y, const char *label, const char *value)
{
    if (!value || !value[0]) return;
    uint16_t dim = curr_colors->dis_c, main_c = curr_colors->main_c;
    char buf[256];
    ui_text_t(16, *y, 100, label, dim);
    ui_ellipsize(buf, sizeof(buf), value, SCR_W - 120 - 12);
    ui_text_t(120, *y, SCR_W - 120 - 12, buf, main_c);
    *y += 15;
}

void ui_info_draw(const player_state_t *ps)
{
    uint16_t accent = curr_colors->sel_c, bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t panel_bg = ui_mix(bg, main_c, 1);
    uint16_t dim = curr_colors->dis_c;
    draw_vbg();
    ui_fill(0, 0, SCR_W, 22, panel_bg);
    ui_fill(0, 21, SCR_W, 1, ui_mix(dim, bg, 5));
    ui_text_t(12, 4, SCR_W - 24, "\xEC\xA0\x95\xEB\xB3\xB4", accent);   // 정보

    const media_tags_t *g = &ps->tags;
    int y = 30;
    info_row(&y, "Title",   ps->title);
    info_row(&y, "Artist",  g->artist);
    info_row(&y, "Album",   g->album);
    info_row(&y, "Album Artist", g->album_artist);
    info_row(&y, "Composer", g->composer);
    info_row(&y, "Genre",   g->genre);
    info_row(&y, "Year",    g->year);
    info_row(&y, "Track",   g->track);
    info_row(&y, "Comment", g->comment);

    y += 4;
    ui_fill(16, y, SCR_W - 32, 1, ui_mix(curr_colors->dis_c, bg, 8));
    y += 6;

    char v[40];
    int total = ps->total;
    snprintf(v, sizeof(v), "%d:%02d", total / 60, total % 60);
    info_row(&y, "Duration", v);
    int br = audio_bitrate_kbps();
    if (br > 0) { snprintf(v, sizeof(v), "%d kbps", br); info_row(&y, "Bitrate", v); }
    int hz = audio_src_hz();
    if (hz > 0) { snprintf(v, sizeof(v), "%d Hz", hz); info_row(&y, "Sample rate", v); }
    int ch = audio_channels();
    if (ch > 0) info_row(&y, "Channels", ch >= 2 ? "Stereo" : "Mono");
    if (ps->file_size > 0) {
        if (ps->file_size >= 1024 * 1024)
            snprintf(v, sizeof(v), "%ld.%ld MB", ps->file_size / (1024 * 1024),
                     (ps->file_size % (1024 * 1024)) * 10 / (1024 * 1024));
        else
            snprintf(v, sizeof(v), "%ld KB", ps->file_size / 1024);
        info_row(&y, "File size", v);
    }

    static const chip_t hints[] = { { "GAME", ICN_NONE }, { "B", ICN_NONE } };
    ui_fill(0, HINT_DIV, SCR_W, SCR_H - HINT_DIV, panel_bg);
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(dim, bg, 5));
    chip_row(226, hints, 2, ui_mix(accent, bg, 9), ui_mix(curr_colors->dis_c, bg, 10), 0);
}

// --- lyrics (parser in media_lyrics.c) --------------------------------------

void ui_lyrics_draw(const player_state_t *ps, const lyrics_t *ly, int top_line, int active)
{
    uint16_t accent = curr_colors->sel_c, bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t panel_bg = ui_mix(bg, main_c, 1);
    uint16_t dim = curr_colors->dis_c;
    uint16_t soft = ui_mix(main_c, bg, 7);
    draw_vbg();

    ui_fill(0, 0, SCR_W, 22, panel_bg);
    ui_fill(0, 21, SCR_W, 1, ui_mix(dim, bg, 5));
    ui_text_t(12, 4, SCR_W - 24, ps->title && ps->title[0] ? ps->title : "Lyrics", accent);

    const int ROW = 17, TOP = 30, BOTTOM = HINT_DIV - 4;
    int rows = (BOTTOM - TOP) / ROW;
    if (ly->n == 0) {
        ui_text_center_t(110, "\xEA\xB0\x80\xEC\x82\xAC \xEC\x97\x86\xEC\x9D\x8C", soft); // 가사 없음
    }
    for (int r = 0; r < rows; r++) {
        int li = top_line + r;
        if (li < 0 || li >= ly->n) continue;
        const char *txt = ly->line[li];
        if (!txt || !txt[0]) continue;
        int y = TOP + r * ROW;
        bool cur = (li == active);
        if (cur) ui_text_bold_center_t(y, txt, accent);
        else     ui_text_center_t(y, txt, soft);
    }

    static const chip_t hints[] = { { "\xE2\x96\xB2\xE2\x96\xBC", ICN_NONE }, { "GAME", ICN_NONE }, { "B", ICN_NONE } };
    ui_fill(0, HINT_DIV, SCR_W, SCR_H - HINT_DIV, panel_bg);
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(dim, bg, 5));
    chip_row(226, hints, 3, ui_mix(accent, bg, 9), ui_mix(curr_colors->dis_c, bg, 10), 0);
}
