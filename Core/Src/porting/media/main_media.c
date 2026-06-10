// Music — a themed, native-feeling MP3 player for the Game & Watch.
//
// Browse /music (falls back to /media), play with an iPod-style now-playing
// screen (dimmed cover backdrop + crisp cover card + title/artist), full tag
// info and lyrics views, favourites (persisted to SD), repeat/shuffle, seek,
// volume and screen-off. Controls are always shown on a beautiful hint bar.
//
// Rendering lives in media_ui.c; ID3 metadata in media_id3.c; the streaming MP3
// engine in media_audio.c; album-art decode in media_cover.c.

#include <odroid_system.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_buttons.h"
#include "appid.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "rg_i18n.h"
#include "gw_audio.h"
#include "common.h"
#include "gui.h"
#include "main_media.h"
#include "media_id3.h"
#include "media_audio.h"
#include "media_cover.h"
#include "media_ui.h"

#define MAX_ENTRIES   512
#define NAME_MAX_LEN  128
#define PATH_MAX_LEN  256
#define ROW_HEIGHT    34
#define HEADER_HEIGHT 20
#define FOOTER_HEIGHT 16
#define LIST_TOP      HEADER_HEIGHT
#define VISIBLE_ROWS  ((GW_LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)
#define THUMB_SZ      28
#define META_N        16
#define FAV_MAX       64

enum { MODE_FOLDER = 0, MODE_FAV = 1 };
enum { VIEW_PLAY = 0, VIEW_INFO = 1, VIEW_LYRICS = 2 };
enum { MENU_INFO = 1, MENU_LYRICS = 2, MENU_CLOSE = 3 };

#define HOLD_MS     300        // press longer than this => seek-scrub
#define SCRUB_STEP  0.010f     // fraction per frame while scrubbing

typedef struct {
    char name[NAME_MAX_LEN];
    bool is_dir;
    bool is_special;           // the "★ Favourites" shortcut row
} media_entry_t;

static media_entry_t entries[MAX_ENTRIES];
static int  entry_count, cursor, scroll;
static char cur_path[PATH_MAX_LEN];
static char g_root[PATH_MAX_LEN] = "/music";
static int  g_mode = MODE_FOLDER;

// favourites (absolute track paths, persisted to "<root>/.favourites")
static char g_fav[FAV_MAX][PATH_MAX_LEN];
static int  g_fav_count;
static char g_fav_file[PATH_MAX_LEN];

// per-track list metadata (thumbnail + title/artist + duration), cached lazily
typedef struct {
    int  idx;
    bool valid, has_art;
    int  dur;
    char title[64], artist[48];
    uint16_t art[THUMB_SZ * THUMB_SZ];
} track_meta_t;
static track_meta_t g_meta[META_N];
static int g_meta_clock;
static media_tags_t g_scan_tags;     // scratch for list metadata

static bool has_ext(const char *name, const char *ext);
static const track_meta_t *meta_get(int entry_idx);

// ---------------------------------------------------------------------------
// directory / favourites scanning
// ---------------------------------------------------------------------------

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES) return RG_SCANDIR_STOP;
    if (!file->is_dir && !has_ext(file->basename, ".mp3")) return RG_SCANDIR_CONTINUE;
    media_entry_t *e = &entries[entry_count++];
    strncpy(e->name, file->basename, NAME_MAX_LEN - 1);
    e->name[NAME_MAX_LEN - 1] = '\0';
    e->is_dir = file->is_dir;
    e->is_special = false;
    return RG_SCANDIR_CONTINUE;
}

static void meta_invalidate(void)
{
    for (int i = 0; i < META_N; i++) g_meta[i].valid = false;
}

static bool scan_folder(void)
{
    entry_count = cursor = scroll = 0;
    meta_invalidate();
    bool ok = rg_storage_scandir(cur_path, scandir_cb, NULL,
        RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_SORT);

    // prepend the favourites shortcut at the music root
    if (strcmp(cur_path, g_root) == 0 && g_fav_count > 0 && entry_count < MAX_ENTRIES) {
        for (int i = entry_count; i > 0; i--) entries[i] = entries[i - 1];
        entry_count++;
        media_entry_t *e = &entries[0];
        snprintf(e->name, NAME_MAX_LEN, "\xE2\x98\x85 \xEC\xA6\x90\xEA\xB2\xA8\xEC\xB0\xBE\xEA\xB8\xB0 (%d)", g_fav_count);
        e->is_dir = false; e->is_special = true;
    }
    return ok;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void scan_favourites(void)
{
    entry_count = cursor = scroll = 0;
    meta_invalidate();
    for (int i = 0; i < g_fav_count && entry_count < MAX_ENTRIES; i++) {
        media_entry_t *e = &entries[entry_count++];
        strncpy(e->name, base_name(g_fav[i]), NAME_MAX_LEN - 1);
        e->name[NAME_MAX_LEN - 1] = '\0';
        e->is_dir = false; e->is_special = false;
    }
}

static void enter_dir(const char *name)
{
    size_t len = strlen(cur_path);
    if (len + 1 + strlen(name) + 1 >= PATH_MAX_LEN) return;
    if (strcmp(cur_path, "/") != 0) strcat(cur_path, "/");
    strcat(cur_path, name);
    scan_folder();
}

// false => already at root (caller exits the app)
static bool go_parent(void)
{
    if (strcmp(cur_path, g_root) == 0) return false;
    char *slash = strrchr(cur_path, '/');
    if (!slash || slash == cur_path) strcpy(cur_path, g_root);
    else *slash = '\0';
    scan_folder();
    return true;
}

static void move_cursor(int delta)
{
    if (entry_count == 0) return;
    cursor += delta;
    if (cursor < 0) cursor = 0;
    if (cursor >= entry_count) cursor = entry_count - 1;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + VISIBLE_ROWS) scroll = cursor - VISIBLE_ROWS + 1;
}

// ---------------------------------------------------------------------------
// favourites persistence
// ---------------------------------------------------------------------------

static void fav_load(void)
{
    g_fav_count = 0;
    snprintf(g_fav_file, sizeof(g_fav_file), "%s/.favourites", g_root);
    FILE *f = fopen(g_fav_file, "r");
    if (!f) return;
    char line[PATH_MAX_LEN];
    while (g_fav_count < FAV_MAX && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n > 0) { strncpy(g_fav[g_fav_count], line, PATH_MAX_LEN - 1);
                     g_fav[g_fav_count][PATH_MAX_LEN - 1] = '\0'; g_fav_count++; }
    }
    fclose(f);
}

static void fav_save(void)
{
    FILE *f = fopen(g_fav_file, "w");
    if (!f) return;
    for (int i = 0; i < g_fav_count; i++) fprintf(f, "%s\n", g_fav[i]);
    fclose(f);
}

static int fav_find(const char *p)
{
    for (int i = 0; i < g_fav_count; i++)
        if (strcmp(g_fav[i], p) == 0) return i;
    return -1;
}
static bool fav_is(const char *p) { return fav_find(p) >= 0; }

static void fav_toggle(const char *p)
{
    int i = fav_find(p);
    if (i >= 0) {
        for (int j = i; j < g_fav_count - 1; j++) strcpy(g_fav[j], g_fav[j + 1]);
        g_fav_count--;
    } else if (g_fav_count < FAV_MAX) {
        strncpy(g_fav[g_fav_count], p, PATH_MAX_LEN - 1);
        g_fav[g_fav_count][PATH_MAX_LEN - 1] = '\0';
        g_fav_count++;
    }
    fav_save();
}

// ---------------------------------------------------------------------------
// playlist mapping (folder mp3s or favourites)
// ---------------------------------------------------------------------------

static bool has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le) return false;
    const char *s = name + ln - le;
    for (size_t i = 0; i < le; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static bool is_track(int i) { return !entries[i].is_dir && !entries[i].is_special; }

static int nth_track(int n)
{
    for (int i = 0; i < entry_count; i++)
        if (is_track(i) && n-- == 0) return i;
    return -1;
}

static int pl_count(void)
{
    if (g_mode == MODE_FAV) return entry_count;
    int c = 0;
    for (int i = 0; i < entry_count; i++) if (is_track(i)) c++;
    return c;
}

static void entry_track_path(int entry_idx, char *out, size_t outsz)
{
    if (g_mode == MODE_FAV) { snprintf(out, outsz, "%s", g_fav[entry_idx]); return; }
    if (strcmp(cur_path, "/") == 0) snprintf(out, outsz, "/%s", entries[entry_idx].name);
    else snprintf(out, outsz, "%s/%s", cur_path, entries[entry_idx].name);
}

static void pl_path(int pi, char *out, size_t outsz)
{
    int e = (g_mode == MODE_FAV) ? pi : nth_track(pi);
    if (e < 0) { out[0] = '\0'; return; }
    entry_track_path(e, out, outsz);
}

static int cursor_to_pl(int cur)
{
    if (g_mode == MODE_FAV) return cur;
    int pi = 0;
    for (int i = 0; i < cur; i++) if (is_track(i)) pi++;
    return pi;
}

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rng(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static int pick_next(int pi, bool shuffle, int repeat)
{
    int n = pl_count();
    if (n <= 0) return -1;
    if (repeat == REPEAT_ONE) return pi;
    if (shuffle) return (int)(rng() % (uint32_t)n);
    if (pi + 1 < n) return pi + 1;
    return (repeat == REPEAT_ALL) ? 0 : -1;
}

static int pick_prev(int pi, int repeat)
{
    int n = pl_count();
    if (n <= 0) return -1;
    if (pi - 1 >= 0) return pi - 1;
    return (repeat == REPEAT_ALL) ? n - 1 : 0;
}

// ---------------------------------------------------------------------------
// browser list rendering
// ---------------------------------------------------------------------------

static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s;
}

static const track_meta_t *meta_get(int entry_idx)
{
    for (int i = 0; i < META_N; i++)
        if (g_meta[i].valid && g_meta[i].idx == entry_idx) return &g_meta[i];

    track_meta_t *m = &g_meta[g_meta_clock];
    g_meta_clock = (g_meta_clock + 1) % META_N;
    m->idx = entry_idx; m->valid = true; m->has_art = false; m->dur = 0;
    m->title[0] = '\0'; m->artist[0] = '\0';

    char path[PATH_MAX_LEN];
    entry_track_path(entry_idx, path, sizeof(path));

    id3_read_tags(path, &g_scan_tags);
    if (g_scan_tags.title[0]) { strncpy(m->title, g_scan_tags.title, sizeof(m->title) - 1); m->title[sizeof(m->title) - 1] = '\0'; }
    if (g_scan_tags.artist[0]) { strncpy(m->artist, g_scan_tags.artist, sizeof(m->artist) - 1); m->artist[sizeof(m->artist) - 1] = '\0'; }
    m->dur = audio_quick_duration(path);
    m->has_art = cover_thumb(path, m->art, THUMB_SZ);
    return m;
}

// When fast-scrolling we skip the (file-I/O heavy) metadata decode and show the
// bare file name, so the list stays smooth; metadata fills in once movement
// settles. Set by the app loop.
static bool g_list_busy;

static const char *strip_ext(const char *name, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", name);
    char *d = strrchr(buf, '.');
    if (d) *d = '\0';
    return buf;
}

// Fill one visible row for ui_list_draw (called only for on-screen rows).
static void list_item_at(int idx, list_item_t *out)
{
    static char title[NAME_MAX_LEN], dur[16];
    media_entry_t *e = &entries[idx];

    if (e->is_special) { out->kind = LIST_SPECIAL; out->title = e->name; return; }
    if (e->is_dir)     { out->kind = LIST_DIR;     out->title = e->name; return; }

    out->kind = LIST_TRACK;
    char p[PATH_MAX_LEN];
    entry_track_path(idx, p, sizeof(p));
    out->fav = fav_is(p);

    if (g_list_busy) {                      // fast scroll: name only, no decode
        out->title = strip_ext(e->name, title, sizeof(title));
        out->art_sz = THUMB_SZ;
        return;
    }

    const track_meta_t *m = meta_get(idx);
    out->title = m->title[0] ? m->title : strip_ext(e->name, title, sizeof(title));
    out->subtitle = m->artist;
    if (m->dur > 0) { snprintf(dur, sizeof(dur), "%d:%02d", m->dur / 60, m->dur % 60); out->duration = dur; }
    out->art = m->has_art ? m->art : NULL;
    out->art_sz = THUMB_SZ;
}

static void draw_list(void)
{
    const char *head = (g_mode == MODE_FAV)
        ? "\xE2\x98\x85 \xEC\xA6\x90\xEA\xB2\xA8\xEC\xB0\xBE\xEA\xB8\xB0"   // ★ 즐겨찾기
        : cur_path;
    list_view_t v = {
        .header = head, .count = entry_count, .cursor = cursor, .scroll = scroll,
        .visible_rows = LIST_VISIBLE_ROWS, .row_h = LIST_ROW_H, .busy = g_list_busy,
    };
    ui_list_draw(&v, list_item_at);
}

// ---------------------------------------------------------------------------
// player menu (favourite / repeat / shuffle / brightness + info / lyrics)
// ---------------------------------------------------------------------------

static player_state_t *g_ps;
static int  g_cover_n;
static bool g_cover_png;
static char fav_v[20], rep_v[20], shf_v[20], bri_v[8];

static void set_fav_v(void)  { snprintf(fav_v, sizeof(fav_v), "%s", g_ps->favorite ? "\xE2\x99\xA5 \xEC\xBC\x9C\xEC\xA7\x90" : "\xEA\xBA\xBC\xEC\xA7\x90"); }
static void set_rep_v(void)  { const char *s = g_ps->repeat == REPEAT_ALL ? "\xEC\xA0\x84\xEC\xB2\xB4" : g_ps->repeat == REPEAT_ONE ? "\xED\x95\x9C\xEA\xB3\xA1" : "\xEB\x81\x84\xEA\xB8\xB0"; snprintf(rep_v, sizeof(rep_v), "%s", s); }
static void set_shf_v(void)  { snprintf(shf_v, sizeof(shf_v), "%s", g_ps->shuffle ? "\xEC\xBC\x9C\xEC\xA7\x90" : "\xEA\xBA\xBC\xEC\xA7\x90"); }
static void set_bri_v(void)  { snprintf(bri_v, sizeof(bri_v), "%d", lcd_backlight_get()); }

static bool fav_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    if (ev == ODROID_DIALOG_PREV || ev == ODROID_DIALOG_NEXT || ev == ODROID_DIALOG_ENTER) {
        fav_toggle(g_ps->path);
        g_ps->favorite = fav_is(g_ps->path);
        set_fav_v();
    }
    return false;
}
static bool rep_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    if (ev == ODROID_DIALOG_NEXT || ev == ODROID_DIALOG_ENTER) g_ps->repeat = (g_ps->repeat + 1) % 3;
    else if (ev == ODROID_DIALOG_PREV) g_ps->repeat = (g_ps->repeat + 2) % 3;
    set_rep_v();
    return false;
}
static bool shf_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    if (ev == ODROID_DIALOG_PREV || ev == ODROID_DIALOG_NEXT || ev == ODROID_DIALOG_ENTER) g_ps->shuffle = !g_ps->shuffle;
    set_shf_v();
    return false;
}
static bool bri_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    int b = lcd_backlight_get();
    if (ev == ODROID_DIALOG_NEXT && b < 9) lcd_backlight_set(b + 1);
    else if (ev == ODROID_DIALOG_PREV && b > 0) lcd_backlight_set(b - 1);
    set_bri_v();
    return false;
}

static void player_repaint(void)
{
    ui_player_static(g_ps, g_cover_n, g_cover_png);
    ui_player_dynamic(g_ps);
}

static int open_menu(player_state_t *ps)
{
    g_ps = ps;
    set_fav_v(); set_rep_v(); set_shf_v(); set_bri_v();
    odroid_dialog_choice_t choices[] = {
        { 10, "\xEC\xA6\x90\xEA\xB2\xA8\xEC\xB0\xBE\xEA\xB8\xB0", fav_v, 1, fav_cb },   // 즐겨찾기
        { 11, "\xEB\xB0\x98\xEB\xB3\xB5", rep_v, 1, rep_cb },                          // 반복
        { 12, "\xEC\x85\x94\xED\x94\x8C", shf_v, 1, shf_cb },                          // 셔플
        { 13, "\xEB\xB0\x9D\xEA\xB8\xB0", bri_v, 1, bri_cb },                          // 밝기
        ODROID_DIALOG_CHOICE_SEPARATOR,
        { MENU_INFO,   "\xEC\xA0\x95\xEB\xB3\xB4", (char *)"", 1, NULL },              // 정보
        { MENU_LYRICS, "\xEA\xB0\x80\xEC\x82\xAC", (char *)"", 1, NULL },              // 가사
        ODROID_DIALOG_CHOICE_SEPARATOR,
        { MENU_CLOSE,  "\xEB\x8B\xAB\xEA\xB8\xB0", (char *)"", 1, NULL },              // 닫기
        ODROID_DIALOG_CHOICE_LAST,
    };
    return odroid_overlay_dialog("\xEC\x9D\x8C\xEC\x95\x85", choices, 0, player_repaint, 0);  // 음악
}

// ---------------------------------------------------------------------------
// now-playing player loop
// ---------------------------------------------------------------------------

static void compose_static(player_state_t *ps)
{
    for (int i = 0; i < 2; i++) {
        ui_player_static(ps, g_cover_n, g_cover_png);
        ui_player_dynamic(ps);
        lcd_swap();
    }
}

static void music_player(int start_pi)
{
    static player_state_t ps;
    static lyrics_t ly;
    static char fallback[NAME_MAX_LEN];

    int count = pl_count();
    if (count <= 0) return;
    int pi = start_pi < 0 ? 0 : (start_pi >= count ? count - 1 : start_pi);

    g_rng ^= dma_counter + (uint32_t)pi + 1u;
    common_emu_state.skip_frames = 0;
    common_emu_state.pause_frames = 0;
    audio_start_playing(AUDIO_BUFFER_LENGTH);

    ps.shuffle = false; ps.repeat = REPEAT_OFF; ps.scrub = -1.0f;
    ps.volume = odroid_audio_volume_get();

    bool reload = true, recompose = false, dirty = true, screen_off = false;
    int  view = VIEW_PLAY, lyr_scroll = 0;
    int  last_sec = -1, last_active = -2, last_top = -999;
    uint32_t played = 0;
    bool lr_down = false; int lr_dir = 0; uint32_t lr_press = 0; bool scrubbing = false; float scrub = 0;

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);

    while (true) {
        if (reload) {
            reload = false; ps.paused = false; played = 0; scrubbing = false; ps.scrub = -1.0f;
            view = VIEW_PLAY; lyr_scroll = 0; dirty = true;

            pl_path(pi, ps.path, sizeof(ps.path));
            id3_read_tags(ps.path, &ps.tags);
            strncpy(fallback, base_name(ps.path), sizeof(fallback) - 1);
            fallback[sizeof(fallback) - 1] = '\0';
            { char *d = strrchr(fallback, '.'); if (d) *d = '\0'; }
            ps.title  = ps.tags.title[0]  ? ps.tags.title  : fallback;
            ps.artist = ps.tags.artist;
            ps.album  = ps.tags.album;
            ps.track_index = pi; ps.track_count = count;
            ps.favorite = fav_is(ps.path);
            ps.file_size = file_size(ps.path);

            if (!ps.tags.has_lyrics) id3_read_lrc(ps.path, ps.tags.lyrics, ID3_LYRICS_MAX);
            lyrics_parse(ps.tags.lyrics, &ly);

            audio_open(ps.path);
            audio_pump(AUDIO_PUMP_TARGET);
            ps.total = audio_duration_sec();
            ps.sec = 0; last_sec = -1; last_active = -2; last_top = -999;
            g_cover_n = cover_load(ps.path, &g_cover_png);
            recompose = true;
        }
        if (recompose) { recompose = false; if (!screen_off) compose_static(&ps); dirty = true; }

        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define P(b) (joy.values[b] && !prev.values[b])

        // wake from screen-off on any key, consuming that press
        if (screen_off) {
            bool any = false;
            for (int b = 0; b < ODROID_INPUT_MAX; b++) if (P(b)) { any = true; break; }
            if (any) { lcd_backlight_on(); screen_off = false; recompose = true; prev = joy; continue; }
        } else {
            if (P(ODROID_INPUT_B)) { break; }
            if (P(ODROID_INPUT_START)) {
                view = (view + 1) % 3;
                if (view == VIEW_PLAY) recompose = true;
                dirty = true;
            }
            if (view == VIEW_PLAY) {
                if (P(ODROID_INPUT_A)) { ps.paused = !ps.paused; dirty = true; }
                if (P(ODROID_INPUT_UP)) { int v = odroid_audio_volume_get(); if (v < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(v + 1); ps.volume = odroid_audio_volume_get(); dirty = true; }
                if (P(ODROID_INPUT_DOWN)) { int v = odroid_audio_volume_get(); if (v > 0) odroid_audio_volume_set(v - 1); ps.volume = odroid_audio_volume_get(); dirty = true; }
                if (P(ODROID_INPUT_SELECT)) { ps.shuffle = !ps.shuffle; dirty = true; }
                if (P(ODROID_INPUT_POWER)) { screen_off = true; lcd_backlight_off(); }
                if (P(ODROID_INPUT_VOLUME)) {
                    int r = open_menu(&ps);
                    if (r == MENU_INFO) view = VIEW_INFO;
                    else if (r == MENU_LYRICS) view = VIEW_LYRICS;
                    recompose = (view == VIEW_PLAY); dirty = true;
                    odroid_input_read_gamepad(&prev); continue;
                }

                // LEFT/RIGHT: tap = prev/next, hold = seek-scrub
                bool nowL = joy.values[ODROID_INPUT_LEFT], nowR = joy.values[ODROID_INPUT_RIGHT];
                bool now = nowL || nowR;
                if (now && !lr_down) { lr_down = true; lr_dir = nowR ? 1 : -1; lr_press = HAL_GetTick(); scrubbing = false; }
                else if (lr_down && now) {
                    if (!scrubbing && HAL_GetTick() - lr_press > HOLD_MS) { scrubbing = true; scrub = ps.total > 0 ? (float)ps.sec / ps.total : 0; }
                    if (scrubbing) { scrub += lr_dir * SCRUB_STEP; if (scrub < 0) scrub = 0; if (scrub > 0.999f) scrub = 0.999f; ps.scrub = scrub; dirty = true; }
                }
                else if (lr_down && !now) {
                    lr_down = false;
                    if (scrubbing) { audio_seek(scrub); played = (uint32_t)(scrub * ps.total * AUDIO_SAMPLE_RATE); scrubbing = false; ps.scrub = -1.0f; dirty = true; }
                    else if (lr_dir > 0) { int n = pick_next(pi, ps.shuffle, REPEAT_ALL); if (n >= 0 && n != pi) { pi = n; reload = true; } }
                    else { if (ps.sec > 3) { audio_seek(0); played = 0; dirty = true; }
                           else { int n = pick_prev(pi, REPEAT_ALL); if (n >= 0 && n != pi) { pi = n; reload = true; } else { audio_seek(0); played = 0; dirty = true; } } }
                }
            } else if (view == VIEW_LYRICS && !ly.synced) {
                if (P(ODROID_INPUT_UP) && lyr_scroll > 0) { lyr_scroll--; dirty = true; }
                if (P(ODROID_INPUT_DOWN) && lyr_scroll < ly.n - 1) { lyr_scroll++; dirty = true; }
            }
        }
        #undef P
        prev = joy;
        if (reload) continue;

        // feed the audio DMA from the decoded ring
        int16_t *buf = audio_get_active_buffer();
        int len = audio_get_buffer_length();
        int32_t vol = common_emu_sound_get_volume();
        for (int i = 0; i < len; i++) {
            int16_t sm = (ps.paused || audio_ring_count() == 0) ? 0 : audio_pull();
            buf[i] = (int16_t)((sm * vol) >> 8);
        }
        if (!ps.paused) { played += len; audio_pump(AUDIO_PUMP_TARGET); }
        ps.sec = (int)(played / AUDIO_SAMPLE_RATE);

        // render the active view
        if (!screen_off) {
            if (view == VIEW_PLAY) {
                if (ps.sec != last_sec || dirty || scrubbing) { last_sec = ps.sec; ui_player_dynamic(&ps); lcd_swap(); }
            } else if (view == VIEW_INFO) {
                if (dirty) { ui_info_draw(&ps); lcd_swap(); }
            } else { // VIEW_LYRICS
                int act = ly.synced ? lyrics_active_line(&ly, (int)((long long)played * 1000 / AUDIO_SAMPLE_RATE)) : -1;
                int top = ly.synced ? (act > 3 ? act - 3 : 0) : lyr_scroll;
                if (act != last_active || top != last_top || dirty) { ui_lyrics_draw(&ps, &ly, top, act); lcd_swap(); last_active = act; last_top = top; }
            }
        }
        dirty = false;
        common_emu_sound_sync(false);

        // auto-advance at end of track
        if (!ps.paused && audio_eof() && audio_ring_count() == 0) {
            int n = pick_next(pi, ps.shuffle, ps.repeat);
            if (n >= 0) { pi = n; reload = true; } else break;
        }
    }

    audio_stop_playing();
    audio_close();
    if (screen_off) lcd_backlight_on();
}

// ---------------------------------------------------------------------------
// app entry + browser loop
// ---------------------------------------------------------------------------

void app_main_media(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;

    odroid_system_init(APPID_HOMEBREW, 48000);

    strcpy(cur_path, "/music");
    g_mode = MODE_FOLDER;
    strcpy(g_root, "/music");
    fav_load();
    if (!scan_folder()) {            // fall back to /media
        strcpy(cur_path, "/media");
        strcpy(g_root, "/media");
        fav_load();
        scan_folder();
    }

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof(prev));
    lcd_clear_buffers();

    bool dirty = true;
    int  settle = 0, held_dir = 0;
    uint32_t held_t0 = 0, held_last = 0;

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define PRESSED(b) (joy.values[b] && !prev.values[b])
        bool moved = false;

        // vertical navigation with hold-to-repeat (accelerating)
        int vdir = joy.values[ODROID_INPUT_UP] ? -1 : joy.values[ODROID_INPUT_DOWN] ? 1 : 0;
        if (vdir) {
            uint32_t now = HAL_GetTick();
            if (held_dir != vdir) {                 // new press
                move_cursor(vdir); moved = true; held_dir = vdir; held_t0 = held_last = now;
            } else {
                uint32_t since = now - held_t0;
                uint32_t iv = since > 1500 ? 30 : since > 350 ? 75 : 0xFFFFFFFFu;  // delay, then accelerate
                if (now - held_last >= iv) { move_cursor(vdir); moved = true; held_last = now; }
            }
        } else {
            held_dir = 0;
        }
        if (PRESSED(ODROID_INPUT_LEFT))  { move_cursor(-LIST_VISIBLE_ROWS); moved = true; }
        if (PRESSED(ODROID_INPUT_RIGHT)) { move_cursor(LIST_VISIBLE_ROWS);  moved = true; }
        if (moved) { dirty = true; settle = 8; }

        if (PRESSED(ODROID_INPUT_A) && entry_count > 0) {
            media_entry_t *e = &entries[cursor];
            if (e->is_special) {
                g_mode = MODE_FAV; scan_favourites(); dirty = true;
            } else if (e->is_dir) {
                enter_dir(e->name); dirty = true;
            } else {
                music_player(cursor_to_pl(cursor));
                odroid_input_read_gamepad(&joy); prev = joy; dirty = true; held_dir = 0; continue;
            }
        }

        if (PRESSED(ODROID_INPUT_B)) {
            if (g_mode == MODE_FAV) { g_mode = MODE_FOLDER; strcpy(cur_path, g_root); scan_folder(); }
            else if (!go_parent()) odroid_system_switch_app(APPID_LAUNCHER);  // noreturn
            dirty = true;
        }
        #undef PRESSED

        prev = joy;
        if (settle > 0 && --settle == 0) dirty = true;   // settled -> upgrade rows to full metadata
        g_list_busy = settle > 0;

        if (dirty) { draw_list(); lcd_swap(); dirty = false; }
        lcd_wait_for_vblank();
    }
}
