// Video app — a /video browser that plays MJPEG-AVI clips. Renders with the
// Music app's shared list UI (ui_list_draw) so the look is pixel-identical:
// system top bar, folder banner, zebra + selection rows, scrollbar, keycap
// footer. Lives in the Music overlay (only one homebrew runs at a time).

#include <odroid_system.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>

#include "main.h"
#include "gw_lcd.h"
#include "appid.h"
#include "rg_storage.h"
#include "rg_i18n.h"
#include "common.h"
#include "gui.h"
#include "music_ui.h"          // ui_list_draw + list_view_t/list_item_t + LIST_*
#include "main_video.h"
#include "video_play.h"

#define MAX_ENTRIES   128
#define NAME_MAX_LEN  128
#define PATH_MAX_LEN  256
#define VIDEO_ROOT    "/video"

typedef struct { char name[NAME_MAX_LEN]; bool is_dir; } vid_entry_t;
static vid_entry_t entries[MAX_ENTRIES];
static int entry_count, cursor, scroll;
static char cur_path[PATH_MAX_LEN];

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
    out->art      = NULL;          // TODO: first-frame thumbnail
    out->art_sz   = 0;
    out->kind     = entries[i].is_dir ? LIST_DIR : LIST_TRACK;
    out->fav = out->playing = out->paused = false;
}

static void draw_list(void)
{
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + LIST_VISIBLE_ROWS) scroll = cursor - LIST_VISIBLE_ROWS + 1;

    list_view_t v;
    memset(&v, 0, sizeof v);
    v.header       = cur_path;
    v.count        = entry_count;
    v.cursor       = cursor;
    v.scroll       = scroll;
    v.visible_rows = LIST_VISIBLE_ROWS;
    v.row_h        = LIST_ROW_H;
    v.empty_hint   = "no videos";
    v.empty_sub    = VIDEO_ROOT;
    ui_list_draw(&v, item_at);
    lcd_swap();
}

static void show_message(const char *msg)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
    ui_text_center_t(GW_LCD_HEIGHT / 2 - 6, msg, curr_colors->main_c);
    ui_text_center_t(GW_LCD_HEIGHT / 2 + 14, "press a key", curr_colors->dis_c);
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
        show_message("unsupported or unreadable file");
}

void app_main_video(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;
    odroid_system_init(APPID_HOMEBREW, 48000);

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
