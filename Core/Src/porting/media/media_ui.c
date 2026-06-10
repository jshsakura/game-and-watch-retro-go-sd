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
#define CARD_SZ  108
#define CARD_X   ((SCR_W - CARD_SZ) / 2)
#define CARD_Y   14
#define TITLE_Y  126            // 2x (24px) title
#define SUB_Y    154
#define PANEL_Y  168
#define PROG_X   14
#define PROG_W   (SCR_W - 2 * PROG_X)
#define PROG_Y   184
#define TIMES_Y  192
#define HINT_DIV 206
#define HINT1_Y  209
#define HINT2_Y  224

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

// Draw `t` centered at `y`, integer-upscaled `scale`x. The 12px bitmap font has
// no anti-aliasing, so each glyph pixel becomes a crisp scale×scale block. The
// 12px source is rendered into the top scratch rows (0..11) — always repainted
// by the dynamic top bar — then block-copied (fg pixels only) to (centered, y).
// Falls back to 1x if the scaled width would not fit the screen.
static void ui_text_scaled_center(int y, const char *t, uint16_t fg, int scale)
{
    int w = i18n_get_text_width(t);
    if (w <= 0) return;
    while (scale > 1 && w * scale > SCR_W - 8) scale--;
    if (scale <= 1) { ui_text_center_t(y, t, fg); return; }

    const uint16_t S = 0x0001;                 // sentinel bg (≈ black, ≠ fg)
    ui_fill(0, 0, w + 2, FONT_H, S);
    ui_text(0, 0, w + 2, t, fg, S);

    uint16_t *fb = lcd_get_active_buffer();
    int dw = w * scale, dh = FONT_H * scale;
    int ox = (SCR_W - dw) / 2, oy = y;
    for (int j = 0; j < dh; j++) {
        int sy = j / scale, dy = oy + j;
        if (dy < 0 || dy >= SCR_H) continue;
        const uint16_t *srow = fb + sy * SCR_W;
        uint16_t *drow = fb + dy * SCR_W;
        for (int i = 0; i < dw; i++) {
            uint16_t px = srow[i / scale];
            int dx = ox + i;
            if (px != S && dx >= 0 && dx < SCR_W) drow[dx] = px;
        }
    }
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

static void draw_volume(int x, int y, int vol)
{
    uint16_t on = curr_colors->sel_c, off = ui_dim(curr_colors->dis_c, 1, 2);
    for (int i = 0; i < 9; i++)
        ui_fill(x + i * 4, y, 3, 8, i < vol ? on : off);
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

    // cover card (shadow + image/placeholder + border)
    ui_fill(CARD_X + 3, CARD_Y + 3, CARD_SZ, CARD_SZ, ui_dim(bg, 1, 3));
    bool card = has_cover && cover_render_card(cover_n, cover_is_png,
                                               CARD_X, CARD_Y, CARD_SZ, CARD_SZ);
    if (!card) {
        ui_fill(CARD_X, CARD_Y, CARD_SZ, CARD_SZ, ui_mix(bg, accent, 3));
        int nw = i18n_get_text_width("\xE2\x99\xAA");   // ♪
        ui_text_t(CARD_X + (CARD_SZ - nw) / 2, CARD_Y + CARD_SZ / 2 - 6,
                  nw + 2, "\xE2\x99\xAA", ui_mix(bg, main_c, 8));
    }
    ui_rect(CARD_X, CARD_Y, CARD_SZ, CARD_SZ, ui_mix(accent, main_c, 4));

    // title (upscaled 2x) + "artist · album"
    ui_text_scaled_center(TITLE_Y, ps->title && ps->title[0] ? ps->title : "(no title)", main_c, 2);

    char sub[200];
    const char *ar = ps->artist ? ps->artist : "";
    const char *al = ps->album ? ps->album : "";
    if (ar[0] && al[0])      snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", ar, al);  // ·
    else if (ar[0])          snprintf(sub, sizeof(sub), "%s", ar);
    else if (al[0])          snprintf(sub, sizeof(sub), "%s", al);
    else                     sub[0] = '\0';
    if (sub[0]) ui_text_center_t(SUB_Y, sub, soft);

    // static transport panel chrome + always-on hint bar (the dynamic layer
    // only repaints the top bar and the transport sub-region above HINT_DIV, so
    // the hint bar is composed once per track and never re-measured per frame).
    ui_fill(0, PANEL_Y, SCR_W, SCR_H - PANEL_Y, bg);
    ui_fill(0, PANEL_Y, SCR_W, 1, ui_mix(accent, bg, 8));
    ui_fill(0, HINT_DIV, SCR_W, 1, ui_mix(curr_colors->dis_c, bg, 8));
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
    bool scrubbing = ps->scrub >= 0.0f;

    // ---- top bar ----
    ui_fill(0, 0, SCR_W, TOPBAR_H, bg);
    ui_fill(0, TOPBAR_H - 1, SCR_W, 1, ui_mix(accent, bg, 8));
    if (ps->paused) icon_pause(8, 4, 9, 10, accent);
    else            icon_play(8, 4, 9, 10, accent);
    char pos[24];
    snprintf(pos, sizeof(pos), "%d / %d", ps->track_index + 1, ps->track_count);
    ui_text(24, 3, 80, pos, soft, bg);
    draw_volume(SCR_W - 9 * 4 - 6, 5, ps->volume);
    ui_text_t(SCR_W - 9 * 4 - 6 - 18, 3, 14, "\xE2\x99\xA5",     // ♥
              ps->favorite ? accent : ui_dim(dim, 1, 2));

    // ---- transport sub-region (above the static hint bar) ----
    ui_fill(0, PANEL_Y, SCR_W, HINT_DIV - PANEL_Y, bg);
    ui_fill(0, PANEL_Y, SCR_W, 1, ui_mix(accent, bg, 8));

    float frac = scrubbing ? ps->scrub
               : (ps->total > 0 ? (float)ps->sec / (float)ps->total : 0.0f);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fillw = (int)(frac * PROG_W);
    ui_fill(PROG_X, PROG_Y, PROG_W, 4, ui_dim(dim, 1, 2));     // track
    ui_fill(PROG_X, PROG_Y, fillw, 4, accent);                 // elapsed
    dot(PROG_X + fillw, PROG_Y + 2, 4, scrubbing ? main_c : accent);  // knob

    char t[16];
    int shown = scrubbing ? (int)(frac * ps->total) : ps->sec;
    fmt_time(t, sizeof(t), shown);
    ui_text(PROG_X, TIMES_Y, 56, t, scrubbing ? accent : soft, bg);
    fmt_time(t, sizeof(t), ps->total - shown);
    char rem[16]; snprintf(rem, sizeof(rem), "-%s", t);
    int rw = i18n_get_text_width(rem);
    ui_text(SCR_W - PROG_X - rw, TIMES_Y, rw + 2, rem, soft, bg);

    // center status: shuffle / repeat
    char st[48]; st[0] = '\0';
    if (ps->shuffle) strcat(st, "\xEC\x85\x94\xED\x94\x8C");          // 셔플
    if (ps->repeat == REPEAT_ALL) { if (st[0]) strcat(st, "  "); strcat(st, "\xEB\xB0\x98\xEB\xB3\xB5"); }       // 반복
    if (ps->repeat == REPEAT_ONE) { if (st[0]) strcat(st, "  "); strcat(st, "\xEB\xB0\x98\xEB\xB3\xB5 1"); }     // 반복 1
    if (st[0]) {
        int w = i18n_get_text_width(st);
        ui_text((SCR_W - w) / 2, TIMES_Y, w + 2, st, accent, bg);
    }
}

// Always-on button hint bar (static layer — composed once per track).
static void draw_player_hints(void)
{
    uint16_t bg = curr_colors->bg_c, main_c = curr_colors->main_c;
    uint16_t accent = curr_colors->sel_c, dim = curr_colors->dis_c;
    uint16_t soft = ui_mix(main_c, bg, 6);
    static const hint_t line1[] = {
        { "A", "\xEC\x9E\xAC\xEC\x83\x9D" },                                     // 재생
        { "\xE2\x97\x80\xE2\x96\xB6", "\xEA\xB3\xA1/\xED\x83\x90\xEC\x83\x89" },  // ◀▶ 곡/탐색
        { "\xE2\x96\xB2\xE2\x96\xBC", "\xEC\x9D\x8C\xEB\x9F\x89" },               // ▲▼ 음량
    };
    static const hint_t line2[] = {
        { "GAME", "\xEC\xA0\x95\xEB\xB3\xB4/\xEA\xB0\x80\xEC\x82\xAC" },          // 정보/가사
        { "TIME", "\xEC\x85\x94\xED\x94\x8C" },                                  // 셔플
        { "PAUSE", "\xEB\xA9\x94\xEB\x89\xB4" },                                 // 메뉴
        { "B", "\xEB\xAA\xA9\xEB\xA1\x9D" },                                     // 목록
    };
    hint_row(HINT1_Y, line1, 3, accent, soft, ui_mix(dim, bg, 6));
    hint_row(HINT2_Y, line2, 4, accent, dim, ui_mix(dim, bg, 6));
}

// --- info screen ------------------------------------------------------------

static void info_row(int *y, const char *label, const char *value)
{
    if (!value || !value[0]) return;
    uint16_t dim = curr_colors->dis_c, main_c = curr_colors->main_c;
    ui_text_t(16, *y, 92, label, dim);
    ui_text_t(110, *y, SCR_W - 110 - 12, value, main_c);
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
    hint_row(HINT1_Y + 6, hints, 2, accent, curr_colors->dis_c, ui_mix(curr_colors->dis_c, bg, 6));
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
    hint_row(HINT1_Y + 6, hints, 3, accent, curr_colors->dis_c, ui_mix(curr_colors->dis_c, bg, 6));
}
