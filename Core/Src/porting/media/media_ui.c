// Drawing layer for the Music app — see media_ui.h.
//
// The now-playing screen is composed in two layers: a per-track STATIC layer
// (dimmed cover backdrop + crisp cover card + title/artist) drawn into both
// framebuffers, and a per-frame DYNAMIC layer (top bar + transport + always-on
// button-hint bar) drawn into the active buffer each iteration. Glyphs come from
// the i18n font (▲▼◀▶ in Geometric Shapes, ♥♪ in Misc Symbols); play/pause,
// the progress knob and volume pips are drawn geometrically.

#include "media_ui.h"
#include "media_cover.h"
#include "media_audio.h"
#include "gw_lcd.h"
#include "gui.h"
#include "rg_i18n.h"
#include <stdio.h>
#include <string.h>

#define SCR_W  GW_LCD_WIDTH
#define SCR_H  GW_LCD_HEIGHT
#define FONT_H 12

// now-playing layout
#define TOPBAR_H 18
#define CARD_SZ  112
#define CARD_X   ((SCR_W - CARD_SZ) / 2)
#define CARD_Y   26
#define TITLE_Y  150
#define SUB_Y    166
#define PANEL_Y  186
#define PROG_X   16
#define TIMES_W  40                       // reserved width for each flanking time label
#define PROG_BAR_X (PROG_X + TIMES_W + 6)  // bar starts after the left time label
#define PROG_W   (SCR_W - 2 * PROG_BAR_X)  // bar width (symmetric margins)
#define PROG_Y   196                       // progress-bar top
#define TIMES_Y  192                       // time labels (vertically centered on bar)
#define HINT_DIV 210
#define HINT1_Y  216

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

// Save the four r×r corners of a rect (before drawing something over it).
static void corners_save(int x, int y, int w, int h, int r, uint16_t *b)
{
    uint16_t *fb = lcd_get_active_buffer();
    #define GETP(px, py) (((px) >= 0 && (px) < SCR_W && (py) >= 0 && (py) < SCR_H) ? fb[(py) * SCR_W + (px)] : 0)
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int k = j * r + i;
            b[0 * r * r + k] = GETP(x + i, y + j);
            b[1 * r * r + k] = GETP(x + w - 1 - i, y + j);
            b[2 * r * r + k] = GETP(x + i, y + h - 1 - j);
            b[3 * r * r + k] = GETP(x + w - 1 - i, y + h - 1 - j);
        }
    #undef GETP
}

// Restore the corner pixels that fall OUTSIDE radius r (rounds the rect by
// putting the saved background back over the square corners).
static void corners_round_restore(int x, int y, int w, int h, int r, const uint16_t *b)
{
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int dx = r - i, dy = r - j;
            if (dx * dx + dy * dy <= r * r) continue;
            int k = j * r + i;
            ui_px(x + i, y + j, b[0 * r * r + k]);
            ui_px(x + w - 1 - i, y + j, b[1 * r * r + k]);
            ui_px(x + i, y + h - 1 - j, b[2 * r * r + k]);
            ui_px(x + w - 1 - i, y + h - 1 - j, b[3 * r * r + k]);
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
// top/bottom bars read as distinct panels instead of melting into a dark bg.
// Works on dark themes (lifts toward the ink) and light themes alike.
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

#define RGB565(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

// Beautiful retro vinyl record placeholder
// Spin state for the no-cover vinyl placeholder (advanced by ui_player_spin()).
static int  g_vinyl_angle;
static bool g_vinyl_active;

// 16-step cos table (Q7, ×128); sin(i) = cos((i+12) & 15)
static const int VINYL_COS[16] = {
    128, 118, 91, 49, 0, -49, -91, -118, -128, -118, -91, -49, 0, 49, 91, 118
};
#define VINYL_SIN(i) VINYL_COS[((i) + 12) & 15]
#define VINYL_R      54                 // outer radius (fits the 112px card)
#define VINYL_STEPS  16                 // angle resolution for the spin

// Spinning vinyl placeholder shown when a track has no (decodable) cover art.
// `angle` advances each frame to rotate the shine + label mark, so the disc
// reads as a turning record. Concentric body is opaque, so re-drawing the whole
// disc each frame needs no background restore.
static void draw_vinyl_placeholder(int cx, int cy, uint16_t accent, uint16_t bg, int angle)
{
    uint16_t slate  = RGB565(26, 28, 36);
    uint16_t groove = RGB565(48, 52, 64);
    uint16_t sheen  = RGB565(150, 162, 188);   // rotating light streak

    dot(cx, cy, VINYL_R,          slate);
    dot(cx, cy, VINYL_R * 90/100, groove);
    dot(cx, cy, VINYL_R * 88/100, slate);
    dot(cx, cy, VINYL_R * 68/100, groove);
    dot(cx, cy, VINYL_R * 66/100, slate);
    dot(cx, cy, VINYL_R * 46/100, groove);
    dot(cx, cy, VINYL_R * 44/100, slate);

    // rotating shine: a light streak across the grooves (+ a fainter opposite one)
    int a = ((angle % VINYL_STEPS) + VINYL_STEPS) % VINYL_STEPS;
    int cs = VINYL_COS[a], sn = VINYL_SIN(a);
    for (int r = VINYL_R * 30/100; r <= VINYL_R * 96/100; r++) {
        int x = cx + r * cs / 128, y = cy + r * sn / 128;
        ui_px(x, y, sheen); ui_px(x + 1, y, sheen);
        int xo = cx - r * cs / 128, yo = cy - r * sn / 128;
        ui_px(xo, yo, ui_mix(sheen, slate, 6));   // dimmer trailing streak
    }

    dot(cx, cy, VINYL_R * 32/100, accent);         // centre label
    // label mark that orbits with the disc, reinforcing the spin
    ui_fill(cx + (VINYL_R*18/100) * cs / 128 - 1,
            cy + (VINYL_R*18/100) * sn / 128 - 1, 3, 3, ui_mix(bg, accent, 6));
    dot(cx, cy, VINYL_R * 8/100, bg);              // spindle hole
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

static void draw_player_hints(void);

// --- now-playing: static layer ----------------------------------------------

void ui_player_static(const player_state_t *ps, int cover_n, bool cover_is_png)
{
    uint16_t main_c = curr_colors->main_c, bg = curr_colors->bg_c;
    uint16_t accent = curr_colors->sel_c;
    uint16_t soft = ui_mix(main_c, bg, 4);          // brighter secondary text
    uint16_t panel_bg = ui_player_surface();        // lifted, accent-tinted

    draw_vbg();
    bool has_cover = cover_render_backdrop(cover_n, cover_is_png);

    // cover card: rounded, soft shadow, art (or placeholder), thin light frame
    const int RX = CARD_X - 1, RY = CARD_Y - 1, RW = CARD_SZ + 2, RH = CARD_SZ + 2, R = 10;
    uint16_t cbuf[4 * 10 * 10];
    corners_save(RX, RY, RW, RH, R, cbuf);

    ui_fill(CARD_X + 5, CARD_Y + CARD_SZ + 2, CARD_SZ - 10, 4, ui_dim(bg, 2, 5)); // soft shadow
    bool card = has_cover && cover_render_card(cover_n, cover_is_png,
                                               CARD_X, CARD_Y, CARD_SZ, CARD_SZ);
    g_vinyl_active = !card;
    if (!card) {
        ui_fill(CARD_X, CARD_Y, CARD_SZ, CARD_SZ, ui_mix(bg, accent, 2));
        draw_vinyl_placeholder(CARD_X + CARD_SZ / 2, CARD_Y + CARD_SZ / 2, accent, bg, g_vinyl_angle);
    }
    ui_rrect(RX, RY, RW, RH, R, ui_mix(accent, bg, 5));                         // rounded frame (brighter)
    corners_round_restore(RX, RY, RW, RH, R, cbuf);                            // round off square corners

    // title (faux-bold) + "artist · album", both ellipsized to fit
    char buf[256];
    ui_ellipsize(buf, sizeof(buf), ps->title && ps->title[0] ? ps->title : "(no title)", SCR_W - 28);
    ui_text_bold_center_t(TITLE_Y, buf, main_c);

    char sub[200];
    const char *ar = ps->artist ? ps->artist : "";
    const char *al = ps->album ? ps->album : "";
    if (ar[0] && al[0])      snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", ar, al);  // ·
    else if (ar[0])          snprintf(sub, sizeof(sub), "%s", ar);
    else if (al[0])          snprintf(sub, sizeof(sub), "%s", al);
    else                     sub[0] = '\0';
    if (sub[0]) { ui_ellipsize(buf, sizeof(buf), sub, SCR_W - 44); ui_text_center_t(SUB_Y, buf, soft); }

    // static hint bar (the dynamic layer repaints only the top bar + transport
    // sub-region above HINT_DIV, so the hint bar is never re-measured per frame).
    ui_fill(0, PANEL_Y, SCR_W, SCR_H - PANEL_Y, panel_bg);
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(accent, bg, 4));
    draw_player_hints();
}

// Is the spinning vinyl placeholder showing (no decodable cover)? The player
// loop uses this to decide whether to animate.
bool ui_player_has_spin(void) { return g_vinyl_active; }

// Advance the spin and redraw just the rotating disc into the active buffer.
// No-op when a real cover is shown. The card frame/backdrop are left intact
// (the disc is opaque and stays within the card), so only the disc is redrawn.
void ui_player_spin(void)
{
    if (!g_vinyl_active) return;
    uint16_t accent = curr_colors->sel_c, bg = curr_colors->bg_c;
    g_vinyl_angle++;
    draw_vinyl_placeholder(CARD_X + CARD_SZ / 2, CARD_Y + CARD_SZ / 2, accent, bg, g_vinyl_angle);
}

// --- now-playing: dynamic layer ---------------------------------------------

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

static void fmt_time(char *out, int n, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

void ui_player_dynamic(const player_state_t *ps)
{
    uint16_t bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t accent = curr_colors->sel_c, dim = curr_colors->dis_c;
    uint16_t soft = ui_mix(main_c, bg, 4);      // secondary text — brighter, legible
    uint16_t bright = ui_mix(main_c, bg, 1);    // near-full ink for key labels
    uint16_t muted = ui_mix(accent, bg, 5);     // active state icons — clearly visible
    uint16_t panel_bg = ui_player_surface();    // lifted, accent-tinted control bar
    bool scrubbing = ps->scrub >= 0.0f;

    // ---- top bar: [▶ 3/12] ............ [셔플 반복 ♥ vol] ----
    ui_fill(0, 0, SCR_W, TOPBAR_H, panel_bg);
    ui_fill(0, TOPBAR_H - 1, SCR_W, 1, ui_mix(accent, bg, 4));
    if (ps->paused) icon_pause(10, 5, 8, 10, accent);
    else            icon_play(10, 5, 8, 10, accent);
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", ps->track_index + 1, ps->track_count);
    ui_text(24, 4, 80, pos, soft, panel_bg);

    int rx = SCR_W - 8;
    rx -= 26; draw_vol_pips(rx, 5, ps->volume);
    rx -= 10;
    rx -= 13; ui_text(rx, 4, 14, "\xE2\x99\xA5", ps->favorite ? accent : ui_mix(dim, bg, 3), panel_bg);   // ♥
    if (ps->repeat != REPEAT_OFF) {
        rx -= 8 + 11; icon_repeat(rx, 5, muted);
        if (ps->repeat == REPEAT_ONE) { ui_text(rx + 11, 4, 8, "1", muted, panel_bg); rx -= 6; }
    }
    if (ps->shuffle) { rx -= 8 + 11; icon_shuffle(rx, 5, muted); }

    // ---- transport sub-region (above the static hint bar) ----
    ui_fill(0, PANEL_Y, SCR_W, HINT_DIV - PANEL_Y, panel_bg);
    ui_fill(0, PANEL_Y, SCR_W, 1, ui_mix(accent, bg, 4)); // top border of bottom panel

    float frac = scrubbing ? ps->scrub
               : (ps->total > 0 ? (float)ps->sec / (float)ps->total : 0.0f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fillw = (int)(frac * PROG_W);
    ui_fill(PROG_BAR_X, PROG_Y, PROG_W, 4, ui_mix(bg, main_c, 5));     // track (clearer)
    ui_fill(PROG_BAR_X, PROG_Y, fillw, 4, accent);                 // elapsed
    dot(PROG_BAR_X + fillw, PROG_Y + 2, 6, ui_mix(accent, bg, 7)); // knob glow
    dot(PROG_BAR_X + fillw, PROG_Y + 2, 4, scrubbing ? main_c : accent);  // knob

    // elapsed / remaining times flank the bar on a single row
    char t[16];
    int shown = scrubbing ? (int)(frac * ps->total) : ps->sec;
    fmt_time(t, sizeof(t), shown);
    ui_text(PROG_X, TIMES_Y, TIMES_W, t, scrubbing ? accent : bright, panel_bg);

    fmt_time(t, sizeof(t), ps->total - shown);
    char rem[16]; snprintf(rem, sizeof(rem), "-%s", t);
    int rw = i18n_get_text_width(rem);
    ui_text(SCR_W - PROG_X - rw, TIMES_Y, rw + 2, rem, soft, panel_bg);
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
    // bright key labels in keycaps + accent function icons
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

    ui_fill(0, 0, SCR_W, SCR_H, bg);

    // header: title + "cur/total"
    ui_fill(0, 0, SCR_W, H, panel_bg);
    char buf[256];
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", v->count ? v->cursor + 1 : 0, v->count);
    int pw = i18n_get_text_width(pos);
    ui_ellipsize(buf, sizeof(buf), v->header ? v->header : "", SCR_W - 16 - pw - 8);
    ui_text(8, 4, SCR_W - 16 - pw - 8, buf, accent, panel_bg);
    ui_text(SCR_W - 8 - pw, 4, pw + 2, pos, soft, panel_bg);
    ui_fill(0, H - 1, SCR_W, 1, ui_mix(accent, bg, 4));

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
        uint16_t rbg = sel ? pill_bg : bg;
        uint16_t txt = fg;
        uint16_t sub = sel ? ui_mix(fg, bg, 4) : ui_mix(fg, bg, 7);   // clearer artist/duration

        int pill_x = 4;
        int pill_y = y + 2;
        int pill_w = right - pill_x;
        int pill_h = RH - 4;
        int pill_r = 6;
        if (sel) {
            ui_fill(pill_x, pill_y, pill_w, pill_h, pill_bg);
            round_corners(pill_x, pill_y, pill_w, pill_h, pill_r, bg);
            ui_rrect(pill_x, pill_y, pill_w, pill_h, pill_r, ui_mix(bg, accent, 10)); // selection border
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

        int dw = it.duration && it.duration[0] ? i18n_get_text_width(it.duration) : 0;
        int textx = tx + TH + 8;
        int textw = right - textx - (dw ? dw + 10 : 6);

        ui_ellipsize(buf, sizeof(buf), it.title ? it.title : "", textw - (it.fav ? 16 : 0));
        ui_text(textx, y + 5, textw, buf, txt, rbg);
        if (it.subtitle && it.subtitle[0]) {
            ui_ellipsize(buf, sizeof(buf), it.subtitle, textw);
            ui_text(textx, y + 21, textw, buf, sub, rbg);
        }
        if (it.fav)
            ui_text(right - dw - 26, y + 5, 14, "\xE2\x99\xA5", sel ? accent : accent, rbg);   // ♥
        if (dw)
            ui_text(right - dw - 2, y + 25, dw + 2, it.duration, sub, rbg);
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
        { "\xE2\x97\x80\xE2\x96\xB6", ICN_NONE }, { "B", ICN_NONE },     // ◀▶
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
