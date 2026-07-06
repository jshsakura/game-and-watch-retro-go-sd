#ifndef STUB_FAV_GUI_H
#define STUB_FAV_GUI_H
/* Minimal launcher GUI stand-in — just enough shape for rg_favorites.c's
 * tab-materialization code to compile/link. Never driven by the tests
 * (they only exercise the file-IO functions), so bodies are stubbed. */
#include <stdint.h>

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

typedef struct {
    const char *text;
    int enabled;
    int id;
    int arg_type;
    void *arg;
} listbox_item_t;

typedef struct {
    listbox_item_t *items;
    int length;
    int cursor;
} listbox_t;

typedef struct {
    char name[64];
    char status[96];
    int16_t header_idx;
    int16_t logo_idx;
    int initialized;
    int is_empty;
    void *arg;
    listbox_t listbox;
    void *event_handler;
} tab_t;

tab_t *gui_add_tab(const char *name, int16_t logo_idx, int16_t header_idx, void *arg, void *event_handler);
tab_t *gui_get_current_tab(void);
void gui_resize_list(tab_t *tab, int new_size);
listbox_item_t *gui_get_selected_item(tab_t *tab);

#endif
