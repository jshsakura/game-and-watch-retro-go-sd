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
#define TOPBAR_H 20
#define CARD_SZ  120
#define CARD_X   ((SCR_W - CARD_SZ) / 2)
#define CARD_Y   24
#define TITLE_Y  150
#define SUB_Y    166
#define PANEL_Y  184
#define PROG_X   18
#define PROG_W   (SCR_W - 2 * PROG_X)
#define PROG_Y   197
#define TIMES_Y  207
#define HINT_DIV 221
#define HINT1_Y  225

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

static void ui_rect(int x, int y, int w, int h, uint16_t c)
{
    ui_fill(x, y, w, 1, c);
    ui_fill(x, y + h - 1, w, 1, c);
    ui_fill(x, y, 1, h, c);
    ui_fill(x + w - 1, y, 1, h, c);
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

// slim rounded volume bar (0..9)
static void draw_vol_bar(int x, int y, int w, int vol)
{
    ui_fill(x, y, w, 3, ui_dim(curr_colors->dis_c, 1, 2));
    int f = w * vol / 9;
    if (f > 0) ui_fill(x, y, f, 3, curr_colors->sel_c);
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

static void draw_player_hints(void);   // defined with hint_row below (static layer)

// --- now-playing: static layer ----------------------------------------------

void ui_player_static(const player_state_t *ps, int cover_n, bool cover_is_png)
{
    uint16_t main_c = curr_colors->main_c, bg = curr_colors->bg_c;
    uint16_t accent = curr_colors->sel_c;
    uint16_t soft = ui_mix(main_c, bg, 6);

    draw_vbg();
    bool has_cover = cover_render_backdrop(cover_n, cover_is_png);

    // cover card: soft shadow, art (or placeholder), thin light frame
    ui_fill(CARD_X + 3, CARD_Y + CARD_SZ + 1, CARD_SZ, 3, ui_dim(bg, 2, 5));   // soft shadow
    bool card = has_cover && cover_render_card(cover_n, cover_is_png,
                                               CARD_X, CARD_Y, CARD_SZ, CARD_SZ);
    if (!card) {
        ui_fill(CARD_X, CARD_Y, CARD_SZ, CARD_SZ, ui_mix(bg, accent, 2));
        int nw = i18n_get_text_width("\xE2\x99\xAA");   // ♪
        ui_text_t(CARD_X + (CARD_SZ - nw) / 2, CARD_Y + CARD_SZ / 2 - 6,
                  nw + 2, "\xE2\x99\xAA", ui_mix(bg, main_c, 7));
    }
    ui_rect(CARD_X - 1, CARD_Y - 1, CARD_SZ + 2, CARD_SZ + 2, ui_mix(main_c, bg, 9));  // soft frame

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
    ui_fill(0, PANEL_Y, SCR_W, SCR_H - PANEL_Y, bg);
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(curr_colors->dis_c, bg, 6));
    draw_player_hints();
}

// --- now-playing: dynamic layer ---------------------------------------------

typedef struct { const char *key; const char *label; } hint_t;

static void hint_row(int y, const hint_t *h, int n, uint16_t keyc, uint16_t labc, uint16_t sepc)
{
    const char *sep = " \xC2\xB7 ";          // " · "
    int sepw = i18n_get_text_width(sep);
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += i18n_get_text_width(h[i].key) + 3 + i18n_get_text_width(h[i].label);
        if (i) total += sepw;
    }
    int x = (SCR_W - total) / 2;
    if (x < 2) x = 2;
    for (int i = 0; i < n; i++) {
        if (i) { ui_text_t(x, y, sepw + 2, sep, sepc); x += sepw; }
        int kw = i18n_get_text_width(h[i].key);
        ui_text_t(x, y, kw + 2, h[i].key, keyc); x += kw + 3;
        int lw = i18n_get_text_width(h[i].label);
        ui_text_t(x, y, lw + 2, h[i].label, labc); x += lw;
    }
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
    uint16_t soft = ui_mix(main_c, bg, 6);
    uint16_t muted = ui_mix(accent, bg, 11);
    bool scrubbing = ps->scrub >= 0.0f;

    // ---- top bar: [▶ 3/12] ............ [셔플 반복 ♥ vol] ----
    ui_fill(0, 0, SCR_W, TOPBAR_H, bg);
    ui_fill(0, TOPBAR_H - 1, SCR_W, 1, ui_mix(dim, bg, 5));
    if (ps->paused) icon_pause(10, 5, 8, 10, soft);
    else            icon_play(10, 5, 8, 10, soft);
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", ps->track_index + 1, ps->track_count);
    ui_text(24, 4, 80, pos, soft, bg);

    int rx = SCR_W - 8;
    rx -= 34; draw_vol_bar(rx, 8, 34, ps->volume);
    rx -= 10;
    rx -= 13; ui_text(rx, 4, 14, "\xE2\x99\xA5", ps->favorite ? accent : ui_dim(dim, 1, 2), bg);   // ♥
    if (ps->repeat != REPEAT_OFF) {
        const char *r = ps->repeat == REPEAT_ONE ? "\xEB\xB0\x98\xEB\xB3\xB51" : "\xEB\xB0\x98\xEB\xB3\xB5"; // 반복 / 반복1
        rx -= 8 + i18n_get_text_width(r); ui_text(rx, 4, 40, r, muted, bg);
    }
    if (ps->shuffle) {
        rx -= 8 + i18n_get_text_width("\xEC\x85\x94\xED\x94\x8C"); ui_text(rx, 4, 30, "\xEC\x85\x94\xED\x94\x8C", muted, bg); // 셔플
    }

    // ---- transport sub-region (above the static hint bar) ----
    ui_fill(0, PANEL_Y, SCR_W, HINT_DIV - PANEL_Y, bg);

    float frac = scrubbing ? ps->scrub
               : (ps->total > 0 ? (float)ps->sec / (float)ps->total : 0.0f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fillw = (int)(frac * PROG_W);
    ui_fill(PROG_X, PROG_Y, PROG_W, 3, ui_dim(dim, 1, 2));     // track
    ui_fill(PROG_X, PROG_Y, fillw, 3, accent);                 // elapsed
    dot(PROG_X + fillw, PROG_Y + 1, 4, scrubbing ? main_c : accent);  // knob

    char t[16];
    int shown = scrubbing ? (int)(frac * ps->total) : ps->sec;
    fmt_time(t, sizeof(t), shown);
    ui_text(PROG_X, TIMES_Y, 56, t, scrubbing ? accent : soft, bg);
    fmt_time(t, sizeof(t), ps->total - shown);
    char rem[16]; snprintf(rem, sizeof(rem), "-%s", t);
    int rw = i18n_get_text_width(rem);
    ui_text(SCR_W - PROG_X - rw, TIMES_Y, rw + 2, rem, soft, bg);
}

// Always-on button hint bar (static layer — composed once per track). One
// elegant, muted line of the primary actions; the rest live in the PAUSE menu.
static void draw_player_hints(void)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t accent = curr_colors->sel_c, dim = curr_colors->dis_c;
    static const hint_t line1[] = {
        { "A", "\xEC\x9E\xAC\xEC\x83\x9D" },                                     // 재생
        { "\xE2\x97\x80\xE2\x96\xB6", "\xEA\xB3\xA1" },                          // ◀▶ 곡
        { "\xE2\x96\xB2\xE2\x96\xBC", "\xEC\x9D\x8C\xEB\x9F\x89" },               // ▲▼ 음량
        { "PAUSE", "\xEB\xA9\x94\xEB\x89\xB4" },                                 // 메뉴
    };
    hint_row(HINT1_Y, line1, 4, ui_mix(accent, bg, 10), ui_mix(dim, bg, 12), ui_mix(dim, bg, 5));
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
    uint16_t soft = ui_mix(fg, bg, 6);
    const int H = LIST_HEADER_H, RH = v->row_h, TH = 28;        // thumb size
    bool has_bar = v->count > v->visible_rows;
    int right = SCR_W - (has_bar ? 8 : 4);                      // content right edge

    ui_fill(0, 0, SCR_W, SCR_H, bg);

    // header: title + "cur/total"
    ui_fill(0, 0, SCR_W, H, bg);
    char buf[256];
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", v->count ? v->cursor + 1 : 0, v->count);
    int pw = i18n_get_text_width(pos);
    ui_ellipsize(buf, sizeof(buf), v->header ? v->header : "", SCR_W - 16 - pw - 8);
    ui_text(8, 4, SCR_W - 16 - pw - 8, buf, accent, bg);
    ui_text(SCR_W - 8 - pw, 4, pw + 2, pos, dim, bg);
    ui_fill(0, H - 1, SCR_W, 1, ui_mix(dim, bg, 5));

    if (v->count == 0)
        ui_text_center_t(H + 16, "(\xEB\xB9\x84\xEC\x96\xB4\xEC\x9E\x88\xEC\x9D\x8C)", soft);   // (비어있음)

    for (int r = 0; r < v->visible_rows; r++) {
        int idx = v->scroll + r;
        if (idx >= v->count) break;
        list_item_t it; memset(&it, 0, sizeof(it));
        item_at(idx, &it);

        int y = H + r * RH;
        bool sel = (idx == v->cursor);
        uint16_t rbg = sel ? accent : bg;
        uint16_t txt = sel ? bg : fg;
        uint16_t sub = sel ? ui_mix(bg, accent, 4) : dim;
        if (sel) {
            ui_fill(0, y, SCR_W, RH, accent);
            ui_fill(0, y, 3, RH, fg);                          // accent edge bar
        }
        int tx = 8, ty = y + (RH - TH) / 2;

        if (it.kind == LIST_SPECIAL) {
            ui_text(tx + 2, y + (RH - 12) / 2, right - tx - 2, it.title, sel ? bg : accent, rbg);
            continue;
        }
        if (it.kind == LIST_DIR) {
            ui_fill(tx, ty + 5, TH, TH - 10, sub);             // folder glyph
            ui_text(tx + TH + 8, y + (RH - 12) / 2, right - (tx + TH + 8), it.title, txt, rbg);
            continue;
        }

        // track row: thumb, title, artist, duration, heart
        if (it.art && it.art_sz > 0) blit_thumb(it.art, it.art_sz, tx, ty);
        else ui_fill(tx, ty, TH, TH, ui_mix(rbg, sub, 6));

        int dw = it.duration && it.duration[0] ? i18n_get_text_width(it.duration) : 0;
        int textx = tx + TH + 8;
        int textw = right - textx - (dw ? dw + 10 : 6);

        ui_ellipsize(buf, sizeof(buf), it.title ? it.title : "", textw - (it.fav ? 16 : 0));
        ui_text(textx, y + 4, textw, buf, txt, rbg);
        if (it.subtitle && it.subtitle[0]) {
            ui_ellipsize(buf, sizeof(buf), it.subtitle, textw);
            ui_text(textx, y + 18, textw, buf, sub, rbg);
        }
        if (it.fav)
            ui_text(right - dw - 26, y + 4, 14, "\xE2\x99\xA5", sel ? bg : accent, rbg);   // ♥
        if (dw)
            ui_text(right - dw - 2, y + 18, dw + 2, it.duration, sub, rbg);
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
    ui_fill(0, SCR_H - LIST_FOOTER_H, SCR_W, LIST_FOOTER_H, bg);
    ui_fill(0, SCR_H - LIST_FOOTER_H, SCR_W, 1, ui_mix(dim, bg, 5));
    static const hint_t fh[] = {
        { "A", "\xEC\x9E\xAC\xEC\x83\x9D" },                                     // 재생
        { "\xE2\x96\xB2\xE2\x96\xBC", "\xEC\x9D\xB4\xEB\x8F\x99" },               // ▲▼ 이동
        { "\xE2\x97\x80\xE2\x96\xB6", "\xED\x8E\x98\xEC\x9D\xB4\xEC\xA7\x80" },   // ◀▶ 페이지
        { "B", "\xEB\x92\xA4\xEB\xA1\x9C" },                                     // 뒤로
    };
    hint_row(SCR_H - LIST_FOOTER_H + 2, fh, 4, ui_mix(accent, bg, 10), ui_mix(dim, bg, 12), ui_mix(dim, bg, 5));
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
    uint16_t accent = curr_colors->sel_c, bg = curr_colors->bg_c;
    draw_vbg();
    ui_fill(0, 0, SCR_W, 22, bg);
    ui_fill(0, 21, SCR_W, 1, ui_mix(accent, bg, 8));
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

    static const hint_t hints[] = {
        { "GAME", "\xEB\x8B\xAB\xEA\xB8\xB0" },   // 닫기 (cycle back)
        { "B", "\xEB\xAA\xA9\xEB\xA1\x9D" },      // 목록
    };
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(curr_colors->dis_c, bg, 8));
    hint_row(226, hints, 2, ui_mix(accent,bg,10), curr_colors->dis_c, ui_mix(curr_colors->dis_c, bg, 6));
}

// --- lyrics (parser in media_lyrics.c) --------------------------------------

void ui_lyrics_draw(const player_state_t *ps, const lyrics_t *ly, int top_line, int active)
{
    uint16_t accent = curr_colors->sel_c, bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t soft = ui_mix(main_c, bg, 7);
    draw_vbg();

    ui_fill(0, 0, SCR_W, 22, bg);
    ui_fill(0, 21, SCR_W, 1, ui_mix(accent, bg, 8));
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

    static const hint_t hints[] = {
        { "\xE2\x96\xB2\xE2\x96\xBC", "\xEC\x8A\xA4\xED\x81\xAC\xEB\xA1\xA4" }, // ▲▼ 스크롤
        { "GAME", "\xEB\x8B\xAB\xEA\xB8\xB0" },                                // 닫기
        { "B", "\xEB\xAA\xA9\xEB\xA1\x9D" },                                   // 목록
    };
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(curr_colors->dis_c, bg, 8));
    hint_row(226, hints, 3, ui_mix(accent,bg,10), curr_colors->dis_c, ui_mix(curr_colors->dis_c, bg, 6));
}
