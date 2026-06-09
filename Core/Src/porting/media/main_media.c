// Media Browser - homebrew app for game-and-watch-retro-go-sd
//
// Browse the SD "/media" folder like a file explorer and open files with viewers.
//   Browser:   D-pad move, A = enter folder / open .txt, B = up / exit at root
//   Text view: Up/Left = prev page, Down/Right/A = next page, B = back
//
// Implemented: folder browser (inc.1), .txt viewer (inc.2).
// Planned: .png/.jpg image viewer, .epub reader, .avi MJPEG player.
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
#include "rg_i18n.h"
#include "main_media.h"

// Localized strings with English fallback (in case a language lacks the key).
#define MEDIA_HINT  ((curr_lang && curr_lang->s_media_hint)  ? curr_lang->s_media_hint  : "A:Open  B:Back")
#define MEDIA_EMPTY ((curr_lang && curr_lang->s_media_empty) ? curr_lang->s_media_empty : "(empty)")

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

// Text viewer (increment 2)
#define TEXT_BUF_MAX    (256 * 1024)  // max file bytes loaded (truncated beyond)
#define VIEW_ROWS       ((GW_LCD_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT)
#define VIEW_USABLE_W   (GW_LCD_WIDTH - 8)

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
            MEDIA_EMPTY, COLOR_TEXT, COLOR_BG);
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
        MEDIA_HINT, COLOR_HEADER, COLOR_BG);
}

// ---------------------------------------------------------------------------
// Text viewer (increment 2): page through a .txt file.
// Manual char-level wrapping + per-line draw, because i18n_draw_text mishandles
// '\n' and i18n_draw_text_line truncates rather than wraps.
// ---------------------------------------------------------------------------

static char text_buf[TEXT_BUF_MAX];
static int  text_len;

static bool has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le)
        return false;
    const char *s = name + ln - le;
    for (size_t i = 0; i < le; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return false;
    }
    return true;
}

static int utf8_len(unsigned char c)
{
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Read file into text_buf, dropping '\r' and mapping other control chars
// (tab etc.) to spaces. Returns sanitized length; oversized files are truncated.
static int load_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int n = (int)fread(text_buf, 1, TEXT_BUF_MAX - 1, f);
    fclose(f);
    if (n < 0)
        n = 0;

    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text_buf[i];
        if (c == '\r')
            continue;
        else if (c == '\n')
            text_buf[w++] = '\n';
        else if (c < 0x20)
            text_buf[w++] = ' ';
        else
            text_buf[w++] = (char)c;
    }
    text_buf[w] = '\0';
    return w;
}

static int char_px_width(const char *p, int blen)
{
    char ch[8];
    memcpy(ch, p, blen);
    ch[blen] = '\0';
    return i18n_get_text_width(ch);
}

// Draw one page starting at byte offset `start`; returns bytes consumed.
static int render_page(int start)
{
    lcd_clear_active_buffer();

    const char *base = text_buf + start;
    const char *p = base;
    char line[256];

    for (int r = 0; r < VIEW_ROWS && *p; r++) {
        int li = 0, w = 0;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c == '\n') { p++; break; }      // end of line, consume newline
            int blen = utf8_len(c);
            int cw = char_px_width(p, blen);
            if (w + cw > VIEW_USABLE_W)
                break;                          // doesn't fit -> wrap (keep char)
            if (li + blen >= (int)sizeof(line) - 1)
                break;
            memcpy(line + li, p, blen);
            li += blen;
            w  += cw;
            p  += blen;
        }
        line[li] = '\0';
        odroid_overlay_draw_text(4, 2 + r * ROW_HEIGHT, GW_LCD_WIDTH - 8,
            line, COLOR_TEXT, COLOR_BG);
    }

    int consumed = (int)(p - base);
    int pct = text_len ? (int)((long)(start + consumed) * 100 / text_len) : 100;
    char foot[32];
    snprintf(foot, sizeof(foot), "B   %d%%", pct);
    odroid_overlay_draw_text(4, GW_LCD_HEIGHT - FOOTER_HEIGHT + 2, GW_LCD_WIDTH - 8,
        foot, COLOR_HEADER, COLOR_BG);
    return consumed;
}

static void view_text(const char *path)
{
    static int back[2048];   // visited page-start offsets (static: keep off the stack)
    int back_n = 0;
    int top = 0, last_top = -1, consumed = 0;

    text_len = load_text_file(path);

    odroid_gamepad_state_t joy, prev;
    odroid_input_read_gamepad(&prev);   // ignore buttons still held from entering

    while (true) {
        wdog_refresh();
        odroid_input_read_gamepad(&joy);
        #define VP(b) (joy.values[b] && !prev.values[b])

        if (VP(ODROID_INPUT_B))
            return;

        if (top != last_top) {
            consumed = render_page(top);
            lcd_swap();
            last_top = top;
        }

        bool more = (top + consumed) < text_len;
        if ((VP(ODROID_INPUT_DOWN) || VP(ODROID_INPUT_RIGHT) || VP(ODROID_INPUT_A)) && more) {
            if (back_n < 2048)
                back[back_n++] = top;
            top += consumed;
        } else if ((VP(ODROID_INPUT_UP) || VP(ODROID_INPUT_LEFT)) && back_n > 0) {
            top = back[--back_n];
        }

        #undef VP
        prev = joy;
        lcd_wait_for_vblank();
    }
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
            if (entry_count > 0) {
                media_entry_t *e = &entries[cursor];
                if (e->is_dir) {
                    enter_dir(e->name);
                } else if (has_ext(e->name, ".txt")) {
                    char path[PATH_MAX_LEN];
                    if (strcmp(cur_path, "/") == 0)
                        snprintf(path, sizeof(path), "/%s", e->name);
                    else
                        snprintf(path, sizeof(path), "%s/%s", cur_path, e->name);
                    view_text(path);
                    // resync input so a button still held on exit isn't re-fired here
                    odroid_input_read_gamepad(&joy);
                    prev = joy;
                    continue;
                }
                // other file types are handled in later increments
            }
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
