// Video app — a /video browser that plays MJPEG-AVI clips with the same retro
// chrome as the Music app (shared top bar, theme colours, font). Lives in the
// Music overlay (only one homebrew runs at a time). See main_video.h.

#include <odroid_system.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>

#include "main.h"
#include "gw_lcd.h"
#include "appid.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "rg_i18n.h"
#include "common.h"
#include "gui.h"
#include "main_video.h"
#include "video_play.h"

#define MAX_ENTRIES   128   // per-folder cap (kept modest — shares overlay BSS)
#define NAME_MAX_LEN  128
#define PATH_MAX_LEN  256
#define ROW_HEIGHT    18
#define LIST_TOP      36
#define FOOTER_H      14

// Drawn in main_music.c (same overlay) — reuse it so the bar is identical.
extern void music_draw_topbar(const char *title, const char *right_label);

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

static int visible_rows(void)
{
    return (GW_LCD_HEIGHT - LIST_TOP - FOOTER_H) / ROW_HEIGHT;
}

static void draw_centered(int y, const char *t, uint16_t fg)
{
    int w = i18n_get_text_width(t);
    i18n_draw_text_line((GW_LCD_WIDTH - w) / 2, y, w + 2, t, fg, 0, 1);
}

static void draw_list(void)
{
    uint16_t bg = curr_colors->bg_c, main_c = curr_colors->main_c, sel = curr_colors->sel_c;
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;

    char right[24];
    snprintf(right, sizeof right, "%d/%d", entry_count ? cursor + 1 : 0, entry_count);
    music_draw_topbar("Video", right);

    int rows = visible_rows();
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + rows) scroll = cursor - rows + 1;

    if (entry_count == 0) {
        draw_centered(LIST_TOP + 30, "no clips in /video", main_c);
    } else {
        for (int r = 0; r < rows && scroll + r < entry_count; r++) {
            int idx = scroll + r;
            int y = LIST_TOP + r * ROW_HEIGHT;
            bool on = (idx == cursor);
            if (on) odroid_overlay_draw_fill_rect(0, y, GW_LCD_WIDTH, ROW_HEIGHT, sel);
            char label[NAME_MAX_LEN + 4];
            snprintf(label, sizeof label, "%s%s",
                     entries[idx].is_dir ? "[" : "\xE2\x96\xB6 ", entries[idx].name);   // ▶
            if (entries[idx].is_dir) strncat(label, "]", sizeof(label) - strlen(label) - 1);
            i18n_draw_text_line(10, y + 3, GW_LCD_WIDTH - 16, label,
                                on ? bg : main_c, on ? sel : bg, on ? 1 : 0);
        }
    }
    i18n_draw_text_line(8, GW_LCD_HEIGHT - FOOTER_H, GW_LCD_WIDTH - 12,
                        "A play   B back", curr_colors->dis_c, bg, 0);
    lcd_swap();
}

static void show_message(const char *msg)
{
    uint16_t bg = curr_colors->bg_c;
    uint16_t *fb = lcd_get_active_buffer();
    for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) fb[i] = bg;
    draw_centered(GW_LCD_HEIGHT / 2 - 6, msg, curr_colors->main_c);
    draw_centered(GW_LCD_HEIGHT / 2 + 14, "press a key", curr_colors->dis_c);
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
    vid_result_t r = video_play(path);
    if (r == VID_UNPLAYABLE)
        show_message("unsupported or unreadable file");
}

void app_main_video(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;
    odroid_system_init(APPID_HOMEBREW, 48000);

    strcpy(cur_path, "/video");
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
            if (strcmp(cur_path, "/video") != 0) {              // up a folder
                char *s = strrchr(cur_path, '/');
                if (s && s != cur_path) *s = '\0'; else strcpy(cur_path, "/video");
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
