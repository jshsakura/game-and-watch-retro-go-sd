// Music — a themed, native-feeling MP3 player for the Game & Watch.
//
// Browse /music on the SD card, play with a Winamp-style now-playing
// "deck" (7-seg LCD time, scrolling title marquee, live spectrum analyzer,
// position slider and volume), full tag info and lyrics views, favourites
// (persisted to SD), repeat/shuffle, seek, volume and screen-off. Controls are
// always shown on a hint bar. Album-art thumbnails appear in the browser list.
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
#include "bitmaps.h"
#include "rg_i18n.h"
#include "gw_audio.h"
#include "common.h"
#include "gui.h"
#include "main_media.h"
#include "media_id3.h"
#include "media_audio.h"
#include "media_cover.h"
#include "media_ui.h"
#include "rg_rtc.h"

#define MAX_ENTRIES   384   // per-folder cap; shares the MEDIA RAM region with code
#define NAME_MAX_LEN  128
#define PATH_MAX_LEN  256
#define ROW_HEIGHT    40
#define HEADER_HEIGHT 18
#define FOOTER_HEIGHT 14
#define LIST_TOP      HEADER_HEIGHT
#define VISIBLE_ROWS  ((GW_LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)
#define THUMB_SZ      34
#define META_N        24   // list thumbnail/meta cache (~2.4KB each) — keep many rows
                           // cached so scrolling back doesn't re-decode covers
                           // (capped by the MEDIA overlay BSS budget)
#define FAV_MAX       64

enum { MODE_FOLDER = 0, MODE_FAV = 1 };
enum { VIEW_PLAY = 0, VIEW_INFO = 1, VIEW_LYRICS = 2 };
// Activation ids returned by the menu — kept clear of the slider/toggle row ids
// (Brightness=0, Volume=1, Favorite=10, Repeat=11, Shuffle=12, Language=14).
enum { MENU_INFO = 20, MENU_LYRICS = 21 };

#define HOLD_MS     300        // press longer than this => seek-scrub
#define SCRUB_STEP  0.010f     // fraction per frame while scrubbing
#define SPIN_DIV    2          // redraw the deck every Nth loop (~30fps analyzer)

// i18n string with an English fallback when a language lacks the key.
#define TR(field, fallback) ((curr_lang && curr_lang->field) ? curr_lang->field : (fallback))

// Shared top bar for the music app, drawn here (not in media_ui) so it can use
// the SAME system chrome as the launcher status bar: the ribbed Game & Watch
// shell strip, the RGW logo, and the system clock + battery. media_ui calls this
// (declared extern there) for both the browser header and the now-playing deck,
// so the homebrew app's top bar is identical to the rest of the device.
#define MEDIA_STATUS_H 33   // == gui.c STATUS_HEIGHT
void media_draw_topbar(const char *title, const char *right_label)
{
    uint16_t main_c = curr_colors->main_c, bg = curr_colors->bg_c, sel = curr_colors->sel_c;
    odroid_overlay_draw_fill_rect(0, 0, GW_LCD_WIDTH, MEDIA_STATUS_H, main_c);
    odroid_overlay_draw_fill_rect(0, 1, GW_LCD_WIDTH, 2, bg);   // shell ribs
    odroid_overlay_draw_fill_rect(0, 4, GW_LCD_WIDTH, 2, bg);
    odroid_overlay_draw_fill_rect(0, 8, GW_LCD_WIDTH, 2, bg);
    odroid_overlay_draw_logo(8, 16, RG_LOGO_RGW, sel);          // Game & Watch logo

    odroid_battery_state_t b = odroid_input_read_battery();
    odroid_overlay_draw_battery(b, GW_LCD_WIDTH - 28, 17);
    odroid_overlay_clock(GW_LCD_WIDTH - 74, 17);

    int rx = GW_LCD_WIDTH - 80;
    if (right_label && right_label[0]) {                        // folder count / track index
        int w = i18n_get_text_width(right_label);
        rx -= w; i18n_draw_text_line(rx, 16, w + 2, right_label, bg, main_c, 1);
    }
    if (title && title[0]) {                                    // 음악 / folder name
        int tx = 42, tw = rx - tx - 6; if (tw < 20) tw = 20;
        i18n_draw_text_line(tx, 16, tw, title, bg, main_c, 1);
    }
}

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

// Background playback state at file scope: persists across the browser and
// now-playing views so music keeps playing while you browse the list.
static player_state_t ps;            // the now-playing state
static lyrics_t       ly;
static char           fallback[NAME_MAX_LEN];
static int            g_play_pi = -1;   // playing playlist index (-1 = nothing playing)
static bool           g_playing;        // a track is loaded and playing
static bool           g_audio_on;       // the audio DMA has been started

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

// Decode at most one row per repaint so a settled list fills in top-to-bottom
// (covers pop in ~a frame apart) instead of freezing while all decode at once.
static int  g_decode_budget;
static bool g_decode_pending;        // set when a row was deferred -> repaint again

static bool has_ext(const char *name, const char *ext);
static const track_meta_t *meta_get(int entry_idx);

// ---------------------------------------------------------------------------
// directory / favourites scanning
// ---------------------------------------------------------------------------

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES) return RG_SCANDIR_STOP;
    // hide hidden/system entries: the thumbnail cache (.mthumb), .favourites,
    // macOS AppleDouble (._*) / .DS_Store, etc. — only music + real folders show.
    if (file->basename[0] == '.') return RG_SCANDIR_CONTINUE;
    if (!file->is_dir && !has_ext(file->basename, ".mp3")
                      && !has_ext(file->basename, ".wav")) return RG_SCANDIR_CONTINUE;
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
        // no ★ in the text — the special row draws a ★ tile (like album art)
        snprintf(e->name, NAME_MAX_LEN, "%s (%d)", TR(s_favorite, "Favorites"), g_fav_count);
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
    if (cursor >= scroll + LIST_VISIBLE_ROWS) scroll = cursor - LIST_VISIBLE_ROWS + 1;
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

static bool g_fav_dirty;   // favourites changed -> the list needs a rescan

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
    g_fav_dirty = true;
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

// Re-scan the current view (so a freshly added/removed ★ favourites shortcut row
// shows up immediately) while keeping the cursor on the same track.
static void rescan_preserve(void)
{
    char keep[PATH_MAX_LEN] = "";
    if (entry_count > 0 && cursor < entry_count && is_track(cursor))
        entry_track_path(cursor, keep, sizeof(keep));
    if (g_mode == MODE_FAV) scan_favourites(); else scan_folder();
    if (keep[0]) {
        for (int i = 0; i < entry_count; i++) {
            if (!is_track(i)) continue;
            char p[PATH_MAX_LEN]; entry_track_path(i, p, sizeof(p));
            if (strcmp(p, keep) == 0) { cursor = i; break; }
        }
    }
    if (cursor < scroll) scroll = cursor;
    else if (cursor >= scroll + LIST_VISIBLE_ROWS) scroll = cursor - LIST_VISIBLE_ROWS + 1;
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

// Cache-only lookup (no decode/evict). Lets the list keep showing full detail
// for already-seen rows while scrolling, instead of blanking to the file name.
static const track_meta_t *meta_peek(int entry_idx)
{
    for (int i = 0; i < META_N; i++)
        if (g_meta[i].valid && g_meta[i].idx == entry_idx) return &g_meta[i];
    return NULL;
}

static const track_meta_t *meta_get(int entry_idx)
{
    const track_meta_t *cached = meta_peek(entry_idx);
    if (cached) return cached;

    if (g_decode_budget <= 0) { g_decode_pending = true; return NULL; }  // defer to next repaint
    g_decode_budget--;

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
    out->art_sz = THUMB_SZ;
    char p[PATH_MAX_LEN];
    entry_track_path(idx, p, sizeof(p));
    out->fav = fav_is(p);
    out->playing = g_playing && strcmp(p, ps.path) == 0;   // center overlay on the live track
    out->paused  = out->playing && ps.paused;              // overlay shows ▶ (paused) vs ❚❚ (playing)

    // While fast-scrolling, use only already-cached metadata (no decode) so the
    // list stays smooth AND cached rows keep their detail instead of blinking;
    // uncached rows show the bare name and fill in once movement settles.
    const track_meta_t *m = g_list_busy ? meta_peek(idx) : meta_get(idx);
    if (!m) { out->title = strip_ext(e->name, title, sizeof(title)); return; }

    out->title = m->title[0] ? m->title : strip_ext(e->name, title, sizeof(title));
    out->subtitle = m->artist;
    if (m->dur > 0) { snprintf(dur, sizeof(dur), "%d:%02d", m->dur / 60, m->dur % 60); out->duration = dur; }
    out->art = m->has_art ? m->art : NULL;
}

static void draw_list(void)
{
    static char favhead[64];
    snprintf(favhead, sizeof(favhead), "\xE2\x98\x85 %s", TR(s_favorite, "Favorites"));

    // Section banner (drawn below the clean top bar, launcher-style): the app
    // name at the root, the folder name in a subfolder. This no longer sits in
    // the system top bar, so it can't crowd the wide G&W logo.
    const char *folder_head;
    if (strcmp(cur_path, g_root) == 0) {
        folder_head = TR(s_music, "Music");
    } else {
        const char *slash = strrchr(cur_path, '/');
        folder_head = (slash && slash[1]) ? slash + 1 : cur_path;
    }
    const char *head = (g_mode == MODE_FAV) ? favhead : folder_head;

    // Empty-state guidance: favourites view tells the user the list is empty;
    // folder view points them at the current folder to drop music files into.
    const char *empty_hint = NULL, *empty_sub = NULL;
    if (entry_count == 0) {
        if (g_mode == MODE_FAV) {
            empty_hint = TR(s_no_favorite, "No favorites yet");
        } else {
            empty_hint = TR(s_empty_music, "Add music files to:");
            empty_sub  = cur_path;
        }
    }

    list_view_t v = {
        .header = head, .count = entry_count, .cursor = cursor, .scroll = scroll,
        .visible_rows = LIST_VISIBLE_ROWS, .row_h = LIST_ROW_H, .busy = g_list_busy,
        .empty_hint = empty_hint, .empty_sub = empty_sub,
    };
    ui_list_draw(&v, list_item_at);
}

// ---------------------------------------------------------------------------
// player menu (favourite / repeat / shuffle / brightness + info / lyrics)
// ---------------------------------------------------------------------------

static player_state_t *g_ps;
static uint32_t g_played;   // total samples played; shared so the menu keeps playback going

static void playback_feed(void);
static bool playback_autoadvance(void);

// Small album-art thumbnail for the Winamp deck — decoded once per track into
// this static buffer (list-sized, cheap) and lent to media_ui via set_cover.
#define DECK_COVER_SZ 56
static uint16_t g_deck_cover[DECK_COVER_SZ * DECK_COVER_SZ];
static bool     g_deck_has_cover;
// Toggle values use language-neutral symbols: ● on / ○ off, ●1 = repeat-one.
#define SYM_ON   "\xE2\x97\x8F"   // ●
#define SYM_OFF  "\xE2\x97\x8B"   // ○

static char fav_v[20], rep_v[20], shf_v[20];

static void set_fav_v(void)  { snprintf(fav_v, sizeof(fav_v), "%s", g_ps->favorite ? SYM_ON : SYM_OFF); }
static void set_rep_v(void)  { snprintf(rep_v, sizeof(rep_v), "%s", g_ps->repeat == REPEAT_ALL ? SYM_ON : g_ps->repeat == REPEAT_ONE ? SYM_ON "1" : SYM_OFF); }
static void set_shf_v(void)  { snprintf(shf_v, sizeof(shf_v), "%s", g_ps->shuffle ? SYM_ON : SYM_OFF); }

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
// Live language switch (◀▶ cycles) — handy from inside the app.
static char lang_v[28];
static void set_lang_v(void) { snprintf(lang_v, sizeof(lang_v), "%s", i18n_lang_display_name(odroid_settings_lang_get())); }
static bool lang_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    int8_t lang = odroid_settings_lang_get();
    if (ev == ODROID_DIALOG_PREV)      lang = odroid_settings_get_prior_lang(lang);
    else if (ev == ODROID_DIALOG_NEXT) lang = odroid_settings_get_next_lang(lang);
    if (ev == ODROID_DIALOG_PREV || ev == ODROID_DIALOG_NEXT) {
        odroid_settings_lang_set(lang);
        curr_lang = i18n_load_language(lang);
    }
    set_lang_v();
    return false;
}

// Launcher-identical Brightness + Volume sliders (bar graph using the font's
// s_Full / s_Fill block glyphs), WITHOUT the Turbo row the system settings menu
// would force in. Both use the 0..9 scale.
#define SLIDER_MAX 9
static char bri_bar[20], vol_bar[20];

static char bar_full(void) { return (curr_lang && curr_lang->s_Full) ? curr_lang->s_Full[0] : '|'; }
static char bar_fill(void) { return (curr_lang && curr_lang->s_Fill) ? curr_lang->s_Fill[0] : '.'; }

static void set_bri_bar(void)
{
    int lvl = odroid_display_get_backlight();
    char full = bar_full(), fill = bar_fill();
    int k = 0;
    for (int i = 0; i <= SLIDER_MAX; i++) bri_bar[k++] = (i <= lvl) ? full : fill;
    bri_bar[k] = '\0';
}
static void set_vol_bar(void)
{
    int lvl = odroid_audio_volume_get();
    char full = bar_full(), fill = bar_fill();
    int k = 0;
    for (int i = ODROID_AUDIO_VOLUME_MIN; i <= ODROID_AUDIO_VOLUME_MAX; i++)
        vol_bar[k++] = ((i - ODROID_AUDIO_VOLUME_MIN) <= lvl) ? full : fill;
    vol_bar[k] = '\0';
}
static bool bri_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    int lvl = odroid_display_get_backlight();
    if (ev == ODROID_DIALOG_PREV && lvl > 0) odroid_display_set_backlight(lvl - 1);
    else if (ev == ODROID_DIALOG_NEXT && lvl < SLIDER_MAX) odroid_display_set_backlight(lvl + 1);
    set_bri_bar();
    return ev == ODROID_DIALOG_ENTER;
}
static bool vol_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    int lvl = odroid_audio_volume_get();
    if (ev == ODROID_DIALOG_PREV && lvl > ODROID_AUDIO_VOLUME_MIN) odroid_audio_volume_set(lvl - 1);
    else if (ev == ODROID_DIALOG_NEXT && lvl < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(lvl + 1);
    set_vol_bar();
    return ev == ODROID_DIALOG_ENTER;
}

// Feed the audio DMA from the decoded ring and advance the play position. Used
// by the menu's repaint callback so music keeps playing (and the analyzer keeps
// dancing) while the blocking dialog is open. One DMA half (~22ms at 48kHz) is
// longer than a menu frame, so filling one half per repaint never underruns.
static void player_audio_service(void)
{
    playback_feed();                // top up the ring + hand the ISR vol/play state
    common_emu_sound_sync(false);   // pace to the DMA (advanced by the ISR)
}

static void player_repaint(void)
{
    player_audio_service();        // keep the music playing while the menu is open
    ui_player_static(g_ps);
    ui_player_dynamic(g_ps);
}

// The deck's options menu: launcher-style Brightness + Volume sliders (no Turbo)
// plus the track actions.
static int open_menu(player_state_t *ps)
{
    g_ps = ps;
    set_fav_v(); set_rep_v(); set_shf_v(); set_bri_bar(); set_vol_bar();
    odroid_dialog_choice_t choices[] = {
        { 0, TR(s_Brightness, "Brightness"), bri_bar, 1, bri_cb },
        { 1, TR(s_Volume,     "Volume"),     vol_bar, 1, vol_cb },
        ODROID_DIALOG_CHOICE_SEPARATOR,
        { 10, TR(s_favorite, "Favorite"), fav_v, 1, fav_cb },
        { 11, TR(s_repeat,   "Repeat"),   rep_v, 1, rep_cb },
        { 12, TR(s_shuffle,  "Shuffle"),  shf_v, 1, shf_cb },
        ODROID_DIALOG_CHOICE_SEPARATOR,
        { MENU_INFO,   TR(s_info,   "Info"),   (char *)"", 1, NULL },
        { MENU_LYRICS, TR(s_lyrics, "Lyrics"), (char *)"", 1, NULL },
        ODROID_DIALOG_CHOICE_LAST,
    };
    return odroid_overlay_dialog(TR(s_music, "Music"), choices, 0, player_repaint, 0);
}

// The browser list's options menu: same system settings menu (so Volume +
// Brightness look and behave exactly like the launcher) plus the live language
// switch. Track-specific items only make sense on the deck, so they're omitted.
static void browser_repaint(void)
{
    if (g_playing) { playback_feed(); common_emu_sound_sync(false); }   // keep music playing
    else lcd_wait_for_vblank();                                          // pace when idle
    draw_list();
}

// selected-track state for the list's "Info / Lyrics / Favorite" items (built
// from tags only — never opens the audio decoder, so background playback is
// untouched).
static player_state_t sel_ps;
static lyrics_t       sel_ly;
static char           sel_fallback[NAME_MAX_LEN];
static char           g_sel_path[PATH_MAX_LEN];
static char           sel_fav_v[20];

static void set_sel_fav_v(void) { snprintf(sel_fav_v, sizeof(sel_fav_v), "%s", fav_is(g_sel_path) ? SYM_ON : SYM_OFF); }
static bool sel_fav_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t ev, uint32_t r)
{
    (void)o; (void)r;
    if (ev == ODROID_DIALOG_PREV || ev == ODROID_DIALOG_NEXT || ev == ODROID_DIALOG_ENTER) {
        fav_toggle(g_sel_path); set_sel_fav_v();
    }
    return false;
}

// The browser list's options menu: launcher-style Brightness + Volume (no Turbo),
// the selected track's Favorite/Info/Lyrics (when a track is highlighted), and
// the live language switch. Returns MENU_INFO / MENU_LYRICS when chosen.
static int open_browser_menu(void)
{
    g_ps = &ps;                 // repeat/shuffle act on the (background) playback state
    set_lang_v(); set_bri_bar(); set_vol_bar(); set_rep_v(); set_shf_v();
    bool is_track = entry_count > 0 && !entries[cursor].is_dir && !entries[cursor].is_special;
    if (is_track) { entry_track_path(cursor, g_sel_path, sizeof(g_sel_path)); set_sel_fav_v(); }

    odroid_dialog_choice_t c[14]; int n = 0;
    c[n++] = (odroid_dialog_choice_t){ 0, TR(s_Brightness, "Brightness"), bri_bar, 1, bri_cb };
    c[n++] = (odroid_dialog_choice_t){ 1, TR(s_Volume,     "Volume"),     vol_bar, 1, vol_cb };
    // same playback options as the now-playing deck when a track is highlighted.
    if (is_track) {
        c[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_SEPARATOR;
        c[n++] = (odroid_dialog_choice_t){ 10, TR(s_favorite, "Favorite"), sel_fav_v, 1, sel_fav_cb };
        c[n++] = (odroid_dialog_choice_t){ 11, TR(s_repeat,   "Repeat"),   rep_v, 1, rep_cb };
        c[n++] = (odroid_dialog_choice_t){ 12, TR(s_shuffle,  "Shuffle"),  shf_v, 1, shf_cb };
        c[n++] = (odroid_dialog_choice_t){ MENU_INFO,   TR(s_info,   "Info"),   (char *)"", 1, NULL };
        c[n++] = (odroid_dialog_choice_t){ MENU_LYRICS, TR(s_lyrics, "Lyrics"), (char *)"", 1, NULL };
    }
    c[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_SEPARATOR;
    c[n++] = (odroid_dialog_choice_t){ 14, TR(s_LangUI, "Language"), lang_v, 1, lang_cb };
    c[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;
    return odroid_overlay_dialog(TR(s_music, "Music"), c, 0, browser_repaint, 0);
}

// Load the highlighted track's tags/lyrics (no audio_open) for the list's Info /
// Lyrics views, then show them until B/A. Background music keeps playing.
static void load_sel_state(void)
{
    memset(&sel_ps, 0, sizeof(sel_ps));
    snprintf(sel_ps.path, sizeof(sel_ps.path), "%s", g_sel_path);
    id3_read_tags(sel_ps.path, &sel_ps.tags);
    strncpy(sel_fallback, base_name(sel_ps.path), sizeof(sel_fallback) - 1);
    sel_fallback[sizeof(sel_fallback) - 1] = '\0';
    { char *d = strrchr(sel_fallback, '.'); if (d) *d = '\0'; }
    sel_ps.title  = sel_ps.tags.title[0] ? sel_ps.tags.title : sel_fallback;
    sel_ps.artist = sel_ps.tags.artist;
    sel_ps.album  = sel_ps.tags.album;
    sel_ps.favorite = fav_is(sel_ps.path);
    sel_ps.file_size = file_size(sel_ps.path);
    sel_ps.scrub = -1.0f;
    if (!sel_ps.tags.has_lyrics) id3_read_lrc(sel_ps.path, sel_ps.tags.lyrics, ID3_LYRICS_MAX);
    lyrics_parse(sel_ps.tags.lyrics, &sel_ly);
}

static void view_selected_track(bool show_lyrics)
{
    load_sel_state();
    int top = 0;
    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);
    for (;;) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define P(b) (joy.values[b] && !prev.values[b])
        bool quit = P(ODROID_INPUT_B) || P(ODROID_INPUT_A);
        if (show_lyrics && sel_ly.n > 0) {
            if (P(ODROID_INPUT_UP)   && top > 0)              top--;
            if (P(ODROID_INPUT_DOWN) && top < sel_ly.n - 1)   top++;
        }
        #undef P
        prev = joy;
        if (quit) break;
        if (g_playing) playback_feed();
        if (show_lyrics) ui_lyrics_draw(&sel_ps, &sel_ly, top, -1);
        else             ui_info_draw(&sel_ps);
        lcd_swap();
        if (g_playing) common_emu_sound_sync(false); else lcd_wait_for_vblank();
    }
}

// ---------------------------------------------------------------------------
// now-playing player loop
// ---------------------------------------------------------------------------

static void compose_static(player_state_t *p)
{
    for (int i = 0; i < 2; i++) {
        ui_player_static(p);
        ui_player_dynamic(p);
        lcd_swap();
    }
}

// Load track `pi` (tags, audio, cover, lyrics) into the shared playback state.
// Used both when entering the deck and when auto-advancing from the list.
static void playback_load(int pi)
{
    g_play_pi = pi;
    g_playing = true;
    ps.paused = false; g_played = 0; ps.scrub = -1.0f;
    ps.app_name = TR(s_music, "Music");
    music_audio_set(0, 0);   // mute the ISR while we reset/reopen the ring (avoids a reset race)
    music_audio_setpos(0);   // new track starts at 0 (the ISR advances from here)

    pl_path(pi, ps.path, sizeof(ps.path));
    id3_read_tags(ps.path, &ps.tags);
    strncpy(fallback, base_name(ps.path), sizeof(fallback) - 1);
    fallback[sizeof(fallback) - 1] = '\0';
    { char *d = strrchr(fallback, '.'); if (d) *d = '\0'; }
    ps.title  = ps.tags.title[0] ? ps.tags.title : fallback;
    ps.artist = ps.tags.artist;
    ps.album  = ps.tags.album;
    ps.track_index = pi; ps.track_count = pl_count();
    ps.favorite = fav_is(ps.path);
    ps.file_size = file_size(ps.path);

    if (!ps.tags.has_lyrics) id3_read_lrc(ps.path, ps.tags.lyrics, ID3_LYRICS_MAX);
    lyrics_parse(ps.tags.lyrics, &ly);

    audio_open(ps.path);
    audio_pump(AUDIO_PUMP_TARGET);
    ps.total = audio_duration_sec();
    ps.bitrate = audio_bitrate_kbps(); ps.hz = audio_src_hz(); ps.channels = audio_channels();
    ps.sec = 0;
    g_deck_has_cover = cover_thumb(ps.path, g_deck_cover, DECK_COVER_SZ);
    ui_player_set_cover(g_deck_cover, DECK_COVER_SZ, g_deck_has_cover);
}

// Keep the (core, gw_audio) ring topped up and hand the SAI ISR the current
// volume / play state + read back the position. The ISR (music_fill) does the
// actual DMA fill, decoupled from rendering, so a slow frame can't starve the
// ~22ms DMA buffer (no more ticks) — and it lives in the main firmware, so the
// audio interrupt never calls into this overlay (no brick).
static void playback_feed(void)
{
    music_audio_set(common_emu_sound_get_volume(), g_playing && !ps.paused);
    if (g_playing && !ps.paused) audio_pump(AUDIO_PUMP_TARGET);
    g_played = music_audio_pos();
    ps.sec = (int)(g_played / AUDIO_SAMPLE_RATE);
}

// Seek to a fraction of the track, muting the ISR while the ring is reset (so
// the core consumer can't race the reset); the next playback_feed un-mutes.
static void seek_to(float frac)
{
    music_audio_set(0, 0);
    audio_seek(frac);
    uint32_t pos = (uint32_t)(frac * ps.total * AUDIO_SAMPLE_RATE);
    music_audio_setpos(pos);
    g_played = pos;
}

// At end of track advance to the next (per shuffle/repeat), or stop at the end
// of the playlist. Returns true when a track change happened.
static bool playback_autoadvance(void)
{
    if (!g_playing || ps.paused) return false;
    if (!(audio_eof() && audio_ring_count() == 0)) return false;
    int n = pick_next(g_play_pi, ps.shuffle, ps.repeat);
    if (n >= 0) { playback_load(n); return true; }
    g_playing = false;
    music_audio_set(0, 0);          // playlist ended -> ISR outputs silence
    return true;
}

static void music_player(int start_pi)
{
    int count = pl_count();
    if (count <= 0) return;
    int pi = start_pi < 0 ? g_play_pi : (start_pi >= count ? count - 1 : start_pi);
    if (pi < 0) pi = 0;

    if (!g_audio_on) {                       // start the DMA once; it runs while the app lives
        g_rng ^= dma_counter + (uint32_t)pi + 1u;
        common_emu_state.skip_frames = 0;
        common_emu_state.pause_frames = 0;
        audio_start_playing(AUDIO_BUFFER_LENGTH);
        music_audio_enable(1);   // Music app now owns the DMA buffer; the ISR feeds it
        g_audio_on = true;
        ps.shuffle = false; ps.repeat = REPEAT_OFF;
    }
    ps.volume = odroid_audio_volume_get();

    // Re-opening the deck for the already-playing track just resumes the view;
    // a different (or freshly picked) track is loaded.
    bool resume = (g_playing && pi == g_play_pi);
    if (resume) pi = g_play_pi;

    bool reload = !resume, recompose = true, dirty = true, screen_off = false;
    int  view = VIEW_PLAY, lyr_scroll = 0;
    int  last_sec = -1, last_active = -2, last_top = -999;
    int  spin_div = 0;                  // throttles the deck animation
    bool lr_down = false; int lr_dir = 0; uint32_t lr_press = 0; bool scrubbing = false; float scrub = 0;

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);

    while (true) {
        if (reload) {
            reload = false; scrubbing = false; ps.scrub = -1.0f;
            view = VIEW_PLAY; lyr_scroll = 0; dirty = true;
            playback_load(pi);
            last_sec = -1; last_active = -2; last_top = -999;
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
            // B: from info/lyrics back to the deck; from the deck, back to the list
            if (P(ODROID_INPUT_B)) {
                if (view != VIEW_PLAY) { view = VIEW_PLAY; recompose = true; dirty = true; }
                else break;
            }
            // PAUSE (the stock retro-go menu hotkey; GAME also works) opens the
            // options menu — volume + brightness sliders plus the track actions.
            // (A toggles play/pause below.)
            if (P(ODROID_INPUT_VOLUME) || P(ODROID_INPUT_START)) {
                int r = open_menu(&ps);
                if (r == MENU_INFO) view = VIEW_INFO;
                else if (r == MENU_LYRICS) view = VIEW_LYRICS;
                recompose = (view == VIEW_PLAY); dirty = true;
                odroid_input_read_gamepad(&prev); continue;
            }
            if (view == VIEW_PLAY) {
                if (P(ODROID_INPUT_A)) { ps.paused = !ps.paused; dirty = true; }
                if (P(ODROID_INPUT_UP)) { int v = odroid_audio_volume_get(); if (v < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(v + 1); ps.volume = odroid_audio_volume_get(); dirty = true; }
                if (P(ODROID_INPUT_DOWN)) { int v = odroid_audio_volume_get(); if (v > 0) odroid_audio_volume_set(v - 1); ps.volume = odroid_audio_volume_get(); dirty = true; }
                if (P(ODROID_INPUT_SELECT)) { ps.shuffle = !ps.shuffle; dirty = true; }
                if (P(ODROID_INPUT_POWER)) { screen_off = true; lcd_backlight_off(); }

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
                    if (scrubbing) { seek_to(scrub); scrubbing = false; ps.scrub = -1.0f; dirty = true; }
                    else if (lr_dir > 0) { int n = pick_next(pi, ps.shuffle, REPEAT_ALL); if (n >= 0 && n != pi) { pi = n; reload = true; } }
                    else { if (ps.sec > 3) { seek_to(0); dirty = true; }
                           else { int n = pick_prev(pi, REPEAT_ALL); if (n >= 0 && n != pi) { pi = n; reload = true; } else { seek_to(0); dirty = true; } } }
                }
            } else if (view == VIEW_LYRICS && !ly.synced) {
                if (P(ODROID_INPUT_UP) && lyr_scroll > 0) { lyr_scroll--; dirty = true; }
                if (P(ODROID_INPUT_DOWN) && lyr_scroll < ly.n - 1) { lyr_scroll++; dirty = true; }
            }
        }
        #undef P
        prev = joy;
        if (reload) continue;

        playback_feed();   // top up the ring + ISR vol/play state + position
        audio_vis_feed();  // feed the analyzer from the play position (smooth)

        // render the active view
        if (!screen_off) {
            if (view == VIEW_PLAY) {
                // The Winamp deck animates continuously while playing (spectrum +
                // marquee scroll), so redraw the dynamic layer on each tick and,
                // between ticks, every SPIN_DIV frames to keep the analyzer alive.
                bool tick = (ps.sec != last_sec || dirty || scrubbing);
                if (tick) { last_sec = ps.sec; spin_div = 0; }
                if (tick || (!ps.paused && ++spin_div >= SPIN_DIV)) {
                    spin_div = 0;
                    ui_player_dynamic(&ps); lcd_swap();
                }
            } else if (view == VIEW_INFO) {
                // render every frame so BOTH framebuffers stay on the info screen
                // (otherwise the idle buffer keeps a stale list/deck image that
                // shows through as a "broken" background).
                ui_info_draw(&ps); lcd_swap();
            } else { // VIEW_LYRICS
                int act = ly.synced ? lyrics_active_line(&ly, (int)((long long)g_played * 1000 / AUDIO_SAMPLE_RATE)) : -1;
                int top = ly.synced ? (act > 3 ? act - 3 : 0) : lyr_scroll;
                ui_lyrics_draw(&ps, &ly, top, act); lcd_swap(); last_active = act; last_top = top;
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

    // NB: audio is NOT stopped here — playback keeps going in the background and
    // the browser loop keeps feeding it (stopped only at end of the playlist).
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
    scan_folder();

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof(prev));
    lcd_clear_buffers();

    bool dirty = true, screen_off = false;
    int  settle = 0, held_dir = 0;
    uint32_t held_t0 = 0, held_last = 0;

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define PRESSED(b) (joy.values[b] && !prev.values[b])

        // screen-off (POWER): blank the backlight; any key wakes it.
        if (screen_off) {
            bool any = false;
            for (int b = 0; b < ODROID_INPUT_MAX; b++) if (PRESSED(b)) { any = true; break; }
            if (any) { lcd_backlight_on(); screen_off = false; dirty = true; }
            prev = joy;
            if (g_playing) { playback_autoadvance(); playback_feed(); common_emu_sound_sync(false); }
            else lcd_wait_for_vblank();
            continue;
        }
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
                if (g_fav_dirty) { rescan_preserve(); g_fav_dirty = false; }   // ★ added on the deck
                odroid_input_read_gamepad(&joy); prev = joy; dirty = true; held_dir = 0; continue;
            }
        }

        if (PRESSED(ODROID_INPUT_B)) {
            if (g_mode == MODE_FAV) { g_mode = MODE_FOLDER; strcpy(cur_path, g_root); scan_folder(); }
            else if (!go_parent()) { music_audio_enable(0); odroid_system_switch_app(APPID_LAUNCHER); }  // noreturn; hand audio back
            dirty = true;
        }

        // PAUSE (the stock retro-go menu hotkey; GAME also works) opens the
        // shared options menu: volume + brightness, plus the selected track's
        // Favorite / Info / Lyrics.
        if (PRESSED(ODROID_INPUT_VOLUME) || PRESSED(ODROID_INPUT_START)) {
            int r = open_browser_menu();
            if (r == MENU_INFO)        view_selected_track(false);
            else if (r == MENU_LYRICS) view_selected_track(true);
            if (g_fav_dirty) { rescan_preserve(); g_fav_dirty = false; }   // show the ★ row at once
            odroid_input_read_gamepad(&joy); prev = joy; dirty = true; held_dir = 0; continue;
        }

        if (PRESSED(ODROID_INPUT_POWER)) {           // blank the screen until any key
            screen_off = true; lcd_backlight_off();
            prev = joy; lcd_wait_for_vblank(); continue;
        }
        #undef PRESSED

        prev = joy;
        if (settle > 0 && --settle == 0) dirty = true;   // settled -> decode any new rows
        // While a track plays, skip the (SD-heavy) thumbnail decode so it can't
        // starve the audio ring — cached rows still show via meta_peek.
        g_list_busy = (settle > 0) || (g_playing && !ps.paused);

        // tick the header clock once a minute even while idle
        { static int lastmin = -1; int m = GW_GetCurrentMinute(); if (m != lastmin) { lastmin = m; dirty = true; } }

        // background playback: auto-advance + keep feeding the DMA, and repaint so
        // the now-playing row's ▶ marker tracks the current track.
        if (g_playing) { if (playback_autoadvance()) dirty = true; playback_feed(); }

        if (dirty) {
            g_decode_budget = 1; g_decode_pending = false;   // one cover per repaint
            draw_list(); lcd_swap();
            dirty = g_decode_pending;                         // more rows? repaint again
        }

        if (g_playing) common_emu_sound_sync(false);   // pace by audio while playing
        else lcd_wait_for_vblank();
    }
}
