// Media Browser - homebrew app for game-and-watch-retro-go-sd
//
// Increment 1 (this file): browse the SD "/media" folder like a file explorer.
//   - D-pad up/down: move cursor, left/right: page
//   - A: enter a folder
//   - B: go to the parent folder, or exit to the launcher at the root
//
// Later increments dispatch by file extension to dedicated viewers:
//   .txt/.epub -> document reader, .png/.jpg -> image viewer, .avi -> MJPEG player.
// See docs/MEDIA_BROWSER.md for the full roadmap.

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
#include "main_media.h"

#define MEDIA_ROOT      "/media"
#define MAX_ENTRIES     256
#define NAME_MAX_LEN    128
#define PATH_MAX_LEN    256

#define ROW_HEIGHT      16
#define HEADER_HEIGHT   20
#define FOOTER_HEIGHT   16
#define LIST_TOP        HEADER_HEIGHT
#define VISIBLE_ROWS    ((GW_LCD_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)

#define COLOR_BG        0x0000  // black
#define COLOR_TEXT      0xFFFF  // white  (files)
#define COLOR_DIR       0xFD20  // orange (folders)
#define COLOR_SEL_BG    0x4208  // dark gray (selected row)
#define COLOR_HEADER    0x07FF  // cyan   (path + hints)

typedef struct {
    char name[NAME_MAX_LEN];
    bool is_dir;
} media_entry_t;

static media_entry_t entries[MAX_ENTRIES];
static int  entry_count;
static int  cursor;   // selected index into entries[]
static int  scroll;   // index of the first visible row
static char cur_path[PATH_MAX_LEN];

static int scandir_cb(const rg_scandir_t *file, void *arg)
{
    (void)arg;
    if (entry_count >= MAX_ENTRIES)
        return RG_SCANDIR_STOP;

    media_entry_t *e = &entries[entry_count++];
    strncpy(e->name, file->basename, NAME_MAX_LEN - 1);
    e->name[NAME_MAX_LEN - 1] = '\0';
    e->is_dir = file->is_dir;
    return RG_SCANDIR_CONTINUE;
}

static void scan_current(void)
{
    entry_count = 0;
    cursor = 0;
    scroll = 0;
    rg_storage_scandir(cur_path, scandir_cb, NULL,
        RG_SCANDIR_FILES | RG_SCANDIR_DIRS | RG_SCANDIR_SORT);
}

static void enter_dir(const char *name)
{
    size_t len = strlen(cur_path);
    if (len + 1 + strlen(name) + 1 >= PATH_MAX_LEN)
        return; // path would overflow; ignore

    if (strcmp(cur_path, "/") != 0)
        strcat(cur_path, "/");
    strcat(cur_path, name);
    scan_current();
}

// Returns false when already at MEDIA_ROOT (caller should exit the app).
static bool go_parent(void)
{
    if (strcmp(cur_path, MEDIA_ROOT) == 0)
        return false;

    char *slash = strrchr(cur_path, '/');
    if (!slash || slash == cur_path)
        strcpy(cur_path, MEDIA_ROOT);
    else
        *slash = '\0';

    scan_current();
    return true;
}

static void move_cursor(int delta)
{
    if (entry_count == 0)
        return;

    cursor += delta;
    if (cursor < 0)             cursor = 0;
    if (cursor >= entry_count)  cursor = entry_count - 1;

    if (cursor < scroll)
        scroll = cursor;
    if (cursor >= scroll + VISIBLE_ROWS)
        scroll = cursor - VISIBLE_ROWS + 1;
}

static void draw(void)
{
    lcd_clear_active_buffer();

    odroid_overlay_draw_text(4, 2, GW_LCD_WIDTH - 8, cur_path, COLOR_HEADER, COLOR_BG);

    if (entry_count == 0) {
        odroid_overlay_draw_text(4, LIST_TOP + 4, GW_LCD_WIDTH - 8,
            "(empty)", COLOR_TEXT, COLOR_BG);
    }

    for (int row = 0; row < VISIBLE_ROWS; row++) {
        int idx = scroll + row;
        if (idx >= entry_count)
            break;

        media_entry_t *e = &entries[idx];
        uint16_t y  = LIST_TOP + row * ROW_HEIGHT;
        bool selected = (idx == cursor);
        uint16_t bg = selected ? COLOR_SEL_BG : COLOR_BG;
        uint16_t fg = e->is_dir ? COLOR_DIR : COLOR_TEXT;

        char line[NAME_MAX_LEN + 4];
        if (e->is_dir)
            snprintf(line, sizeof(line), "[%s]", e->name);
        else
            snprintf(line, sizeof(line), " %s", e->name);

        odroid_overlay_draw_text(4, y, GW_LCD_WIDTH - 8, line, fg, bg);
    }

    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        "A:open   B:back", COLOR_HEADER, COLOR_BG);
}

void app_main_media(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
    (void)save_slot;

    odroid_system_init(APPID_HOMEBREW, 48000);

    strcpy(cur_path, MEDIA_ROOT);
    scan_current();

    odroid_gamepad_state_t joy, prev;
    memset(&prev, 0, sizeof(prev));

    lcd_clear_buffers();

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);

        #define PRESSED(btn) (joy.values[btn] && !prev.values[btn])

        if (PRESSED(ODROID_INPUT_UP))    move_cursor(-1);
        if (PRESSED(ODROID_INPUT_DOWN))  move_cursor(1);
        if (PRESSED(ODROID_INPUT_LEFT))  move_cursor(-VISIBLE_ROWS);
        if (PRESSED(ODROID_INPUT_RIGHT)) move_cursor(VISIBLE_ROWS);

        if (PRESSED(ODROID_INPUT_A)) {
            if (entry_count > 0 && entries[cursor].is_dir)
                enter_dir(entries[cursor].name);
            // file open is handled in later increments (dispatch by extension)
        }

        if (PRESSED(ODROID_INPUT_B)) {
            if (!go_parent())
                odroid_system_switch_app(APPID_LAUNCHER); // noreturn
        }

        #undef PRESSED

        prev = joy;

        draw();
        lcd_swap();
        lcd_wait_for_vblank();
    }
}
