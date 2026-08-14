#pragma once

#include <odroid_input.h>
#include "bitmaps.h"
#include "rg_emulators.h"
#include "stdbool.h"

/* Launcher chrome: the status bar along the top and the header bar along the
 * bottom. Whatever is left between them is the list viewport — which the system
 * grid borrows, so both it and gui.c have to agree on these. */
#define RG_STATUS_HEIGHT (33)
#define RG_HEADER_HEIGHT (47)

typedef enum {
    KEY_PRESS_A,
    KEY_PRESS_B,
    TAB_SCROLL,
    TAB_INIT,
    TAB_REFRESH_LIST,
    TAB_SAVE,
    TAB_IDLE,
    TAB_REDRAW,
} gui_event_t;

typedef enum {
    LINE_UP,
    LINE_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    FIRST_ROW,
    LAST_ROW,
} scroll_mode_t;

typedef struct {
    //char name[24];
    uint16_t bg_c;
    uint16_t main_c;
    uint16_t sel_c;
    uint16_t dis_c;
} colors_t;

typedef struct {
    const char * text;
    int enabled;
    int id;
    int arg_type;
    void *arg;
} listbox_item_t;

typedef struct {
    // listbox_item_t **items;
    listbox_item_t *items;
    int length;
    int cursor;
} listbox_t;

typedef void (*gui_event_handler_t)(gui_event_t event, void *arg);

typedef struct {
    char name[64];
    char status[96];
    int16_t header_idx;
    int16_t logo_idx;
    bool initialized;
    bool is_empty;
    void *arg;
    listbox_t listbox;
    gui_event_handler_t event_handler;
} tab_t;

/*
 * Every tab the launcher can hold: one per emulator, plus the tabs that are
 * not emulators (Favorites, registered directly by rg_favorites_register_tab).
 *
 * This was a bare 32 while there were 32 emulators AND a Favorites tab -- 33
 * gui_add_tab() calls into a 32-slot array. The 33rd wrote the tab pointer
 * straight into `tabcount`, which sits immediately after this array, and the
 * next instruction wrote the correct count back over it (verified in the
 * disassembly: `str r4,[r3,r2,lsl#2]` then `str r1,[r3,#128]`). So the count
 * looked right and nothing crashed -- gui.tabs[32] simply read back as the
 * integer 33, used as a tab_t *, and the LAST system in the list (ZX Spectrum,
 * alphabetically last) appeared as a nameless, iconless blank.
 *
 * Worse than cosmetic: that fake tab points at address 0x21, in ITCM, and
 * gui_init_tab() and the tab event handlers WRITE through it (`initialized`,
 * `status[96]`), landing on 0x61-0xC5 -- which is inside the SNES core's
 * ITCM-resident interpreter (.itcm_snes_interp starts at ORIGIN+4).
 *
 * MAX_EMULATOR_TABS is the other half of this number and must equal
 * rg_emulators.c's MAX_EMULATORS; a _Static_assert there ties the two together
 * so they cannot drift apart again silently, which is how this happened.
 */
#define MAX_EMULATOR_TABS      33   /* == MAX_EMULATORS; 32->33 restores Sega 32X (docs/32X_CLOSED.md) */
#define GUI_NON_EMULATOR_TABS  1     /* Favorites */
#define MAX_TABS               (MAX_EMULATOR_TABS + GUI_NON_EMULATOR_TABS)

typedef struct {
    tab_t *tabs[MAX_TABS];
    int tabcount;
    int selected;
    int theme;
    int show_empty;
    int show_cover;
    int idle_start;
    int last_key;
    odroid_gamepad_state_t joystick;
} retro_gui_t;

extern retro_gui_t gui;
extern int gui_colors_count;
extern colors_t *curr_colors;
extern colors_t gui_colors[];

tab_t *gui_add_tab(const char *name, int16_t logo_idx, int16_t header_idx, void *arg, void *event_handler);
tab_t *gui_get_tab(int index);
tab_t *gui_get_current_tab();
tab_t *gui_set_current_tab(int index);
/** Move to the next (+1) or previous (-1) non-empty tab. */
bool gui_change_tab(int direction);

/* Drop every cover buffer taken from the ram_malloc bump pool. Call whenever
 * that pool is rewound (emulator_start), exactly as rg_reset_logo_buffers() is:
 * the pointers survive the rewind but the memory behind them does not. */
void gui_reset_cover_buffers(void);
/* Invalidate the per-tab cached cover dimensions after the "Cover style"
 * option (Poster/Square) changes, so every tab re-probes its layout size.
 * COVERFLOW builds only. */
void gui_cover_style_changed(void);

void gui_init_tab(tab_t *tab);
void gui_init_colors(void);
void gui_apply_colors_to_overlay_clut(void);
void gui_refresh_tab(tab_t *tab);
void gui_save_current_tab(void);

void gui_sort_list(tab_t *tab, int sort_mode);
void gui_scroll_list(tab_t *tab, scroll_mode_t mode);
void gui_resize_list(tab_t *tab, int new_size);
listbox_item_t *gui_get_selected_item(tab_t *tab);

/** Blit a colour console icon, skipping its transparent pixels. (x, y) is the
 * icon's nominal footprint; the stored bitmap covers only its opaque bbox. */
void gui_draw_color_icon(int x, int y, const color_icon_t *ic);

/** Same, but faded `strength` percent of the way from its own colours toward
 * `toward` (pass the theme's background). Fading toward the ground is what makes
 * an icon recede on a LIGHT theme as well as a dark one — darkening only works
 * on the dark ones. 16 blends for the whole icon, not one per pixel. */
void gui_draw_color_icon_fade(int x, int y, const color_icon_t *ic,
                              uint16_t toward, int strength);

void gui_event(gui_event_t event, tab_t *tab);
/** Pop one ROM browse level if tab is inside a subfolder; refreshes list. */
bool rg_emulator_browse_pop_if_in_subfolder(tab_t *tab);
/** True when ROM list is browsing below /roms/<system>/ (not root of that system). */
bool rg_emulator_tab_in_rom_subfolder(const tab_t *tab);
bool rg_emulator_validate_browse_path_for_tab(tab_t *tab);
void gui_redraw_callback(void);
void gui_redraw(void);
void gui_draw_header(tab_t *tab);
void gui_draw_status(tab_t *tab);
void gui_draw_list(tab_t *tab);
void gui_draw_notice(const char *text, uint16_t color);
void gui_jump_list(tab_t *tab, int offset);
