// Video app — a /video browser that plays MJPEG-AVI clips. Renders with the
// Music app's shared list UI (ui_list_draw) so the look is pixel-identical:
// system top bar, folder banner, zebra + selection rows, scrollbar, keycap
// footer. Each clip shows a generic "video" tile (a play triangle) where the
// Music app shows album art. Lives in the Music overlay (only one homebrew
// runs at a time).

#include <odroid_system.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>

#include "main.h"
#include "gw_lcd.h"
#include "appid.h"
#include "rg_storage.h"
#include "rg_i18n.h"           // curr_lang (active language -> localized strings)
#include "common.h"
#include "gui.h"               // curr_colors
#include "music_ui.h"          // ui_list_draw + list_view_t/list_item_t + LIST_* + ui_dim
#include "main_video.h"
#include "video_play.h"

#define MAX_ENTRIES   128
#define NAME_MAX_LEN  128
#define PATH_MAX_LEN  256
#define VIDEO_ROOT    "/video"
#define TILE_SZ       34          // == ui_list_draw's album-art tile (TH)

typedef struct { char name[NAME_MAX_LEN]; bool is_dir; } vid_entry_t;
static vid_entry_t entries[MAX_ENTRIES];
static int entry_count, cursor, scroll;
static char cur_path[PATH_MAX_LEN];
static uint16_t icon_tile[TILE_SZ * TILE_SZ];   // generic video tile (built once)

// --- localization -----------------------------------------------------------
// Strings are chosen by the ACTIVE language (curr_lang), whose font/codepage is
// already loaded, so the localized glyphs render correctly. No lang_t fields are
// added — that keeps the app decoupled from the SD /lang bins and avoids the
// version-skew brick risk documented in rg_i18n.h. English is the fallback.
typedef enum { VS_APP, VS_EMPTY, VS_UNPLAYABLE, VS_ANYKEY } vstr_t;

static const char *vstr(vstr_t s)
{
    const char *ln = (curr_lang && curr_lang->s_LangName) ? curr_lang->s_LangName : "English";
    bool ko = strcmp(ln, "Korean") == 0;
    bool ja = strcmp(ln, "Japanese") == 0;
    bool zh_cn = strcmp(ln, "Simplified Chinese") == 0;
    bool zh_tw = strcmp(ln, "Traditional Chinese") == 0;
    switch (s) {
    case VS_APP:        return ko ? "비디오" : ja ? "ビデオ"
                             : zh_cn ? "视频" : zh_tw ? "視訊" : "Video";
    case VS_EMPTY:      return ko ? "영상을 넣어주세요:" : "Add video files to:";
    case VS_UNPLAYABLE: return ko ? "재생할 수 없는 파일입니다" : "unsupported or unreadable file";
    case VS_ANYKEY:     return ko ? "아무 키나 누르세요" : "press a key";
    }
    return "";
}

// Build the generic video tile once: a dark themed square, a 1px accent frame,
// and a centered white play triangle — the "this is a video" stand-in for cover
// art. Theme is fixed for the app's lifetime, so once is enough.
static void build_icon_tile(void)
{
    uint16_t base  = ui_dim(curr_colors->sel_c, 2, 5);   // medium-dark accent
    uint16_t frame = curr_colors->sel_c;                 // accent border
    for (int i = 0; i < TILE_SZ * TILE_SZ; i++) icon_tile[i] = base;
    for (int i = 0; i < TILE_SZ; i++) {
        icon_tile[i] = frame;                            // top
        icon_tile[(TILE_SZ - 1) * TILE_SZ + i] = frame;  // bottom
        icon_tile[i * TILE_SZ] = frame;                  // left
        icon_tile[i * TILE_SZ + TILE_SZ - 1] = frame;    // right
    }
    // right-pointing play triangle, vertically centered
    const int xl = 13, xr = 25, ytop = 9, ybot = 25;
    const int ymid = (ytop + ybot) / 2, half = (ybot - ytop) / 2;
    for (int y = ytop; y <= ybot; y++) {
        int dy = y > ymid ? y - ymid : ymid - y;         // 0..half
        int rx = xl + (xr - xl) * (half - dy) / half;
        for (int x = xl; x <= rx; x++) icon_tile[y * TILE_SZ + x] = 0xFFFF;
    }
}

// --- directory scan ---------------------------------------------------------

static bool has_avi_ext(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (strcasecmp(d, ".avi") == 0);
}

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES) return RG_SCANDIR_STOP;
    if (file->basename[0] == '.') return RG_SCANDIR_CONTINUE;
    if (!file->is_dir && !has_avi_ext(file->basename)) return RG_SCANDIR_CONTINUE;
    vid_entry_t *e = &entries[entry_count++];
    strncpy(e->name, file->basename, NAME_MAX_LEN - 1);
    e->name[NAME_MAX_LEN - 1] = '\0';
    e->is_dir = file->is_dir;
    return RG_SCANDIR_CONTINUE;
}

static void scan(void)
{
    entry_count = cursor = scroll = 0;
    rg_storage_scandir(cur_path, scandir_cb, NULL,
        RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_SORT);
}

// Fill one visible row for ui_list_draw (called only for on-screen rows).
static void item_at(int i, list_item_t *out)
{
    out->title    = entries[i].name;
    out->subtitle = "";
    out->duration = "";
    out->kind     = entries[i].is_dir ? LIST_DIR : LIST_TRACK;
    out->art      = entries[i].is_dir ? NULL : icon_tile;   // video tile on files
    out->art_sz   = entries[i].is_dir ? 0    : TILE_SZ;
    out->fav = out->playing = out->paused = false;
}

static void draw_list(void)
{
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + LIST_VISIBLE_ROWS) scroll = cursor - LIST_VISIBLE_ROWS + 1;

    // Banner shows the localized app name at the root, the folder name deeper in
    // (mirrors the Music browser), so a Korean user sees "비디오", not "/video".
    const char *header;
    if (strcmp(cur_path, VIDEO_ROOT) == 0) {
        header = vstr(VS_APP);
    } else {
        const char *slash = strrchr(cur_path, '/');
        header = (slash && slash[1]) ? slash + 1 : cur_path;
    }

    list_view_t v;
    memset(&v, 0, sizeof v);
    v.header       = header;
    v.count        = entry_count;
    v.cursor       = cursor;
    v.scroll       = scroll;
    v.visible_rows = LIST_VISIBLE_ROWS;
    v.row_h        = LIST_ROW_H;
    v.empty_hint   = vstr(VS_EMPTY);
    v.empty_sub    = cur_path;
    ui_list_draw(&v, item_at);
    lcd_swap();
}

static void show_message(const char *msg)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
    ui_text_center_t(GW_LCD_HEIGHT / 2 - 6, msg, curr_colors->main_c);
    ui_text_center_t(GW_LCD_HEIGHT / 2 + 14, vstr(VS_ANYKEY), curr_colors->dis_c);
    lcd_swap();
    odroid_gamepad_state_t j;
    for (;;) {
        wdog_refresh();
        odroid_input_read_gamepad(&j);
        int any = 0;
        for (int b = 0; b < ODROID_INPUT_MAX; b++) if (j.values[b]) any = 1;
        if (any) break;
        HAL_Delay(20);
    }
    HAL_Delay(150);
}

static void enter_selected(void)
{
    vid_entry_t *e = &entries[cursor];
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/%s", cur_path, e->name);
    if (e->is_dir) {
        snprintf(cur_path, sizeof cur_path, "%s", path);
        scan();
        return;
    }
    if (video_play(path) == VID_UNPLAYABLE)
        show_message(vstr(VS_UNPLAYABLE));
}

void app_main_video(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;
    odroid_system_init(APPID_HOMEBREW, 48000);

    build_icon_tile();
    strcpy(cur_path, VIDEO_ROOT);
    scan();

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof prev);
    lcd_clear_buffers();
    bool dirty = true;

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define HIT(b) (joy.values[b] && !prev.values[b])

        if (HIT(ODROID_INPUT_DOWN) && cursor + 1 < entry_count) { cursor++; dirty = true; }
        if (HIT(ODROID_INPUT_UP)   && cursor > 0)               { cursor--; dirty = true; }
        if (HIT(ODROID_INPUT_A) && entry_count > 0)             { enter_selected(); dirty = true; }
        if (HIT(ODROID_INPUT_B)) {
            if (strcmp(cur_path, VIDEO_ROOT) != 0) {            // up a folder
                char *s = strrchr(cur_path, '/');
                if (s && s != cur_path) *s = '\0'; else strcpy(cur_path, VIDEO_ROOT);
                scan(); dirty = true;
            } else {
                odroid_system_switch_app(APPID_LAUNCHER);       // noreturn
            }
        }

        prev = joy;
        if (dirty) { draw_list(); dirty = false; }
        HAL_Delay(16);
    }
}
