/* Host-test stub. Trimmed shape of retro-go-stm32/components/odroid/odroid_overlay.h
 * — just the dialog-choice type + the one entry point video_play.c calls
 * (the PAUSE/SET options menu). */
#ifndef STUB_ODROID_OVERLAY_H
#define STUB_ODROID_OVERLAY_H
#include <stdint.h>
#include <stdbool.h>
#include "odroid_input.h"

typedef enum {
    ODROID_DIALOG_INIT, ODROID_DIALOG_PREV, ODROID_DIALOG_NEXT,
    ODROID_DIALOG_FOCUS_GAINED, ODROID_DIALOG_ENTER,
} odroid_dialog_event_t;

typedef enum { ODROID_MENU_FLAG_DRAW_ONLY = 1 << 0, ODROID_MENU_FLAG_NO_BG_DARKEN = 1 << 1 } odroid_menu_flags_t;

typedef struct odroid_dialog_choice odroid_dialog_choice_t;
struct odroid_dialog_choice {
    int id;
    const char *label;
    char *value;
    int enabled;
    bool (*update_cb)(odroid_dialog_choice_t *, odroid_dialog_event_t, uint32_t repeat);
};
typedef void (*void_callback_t)();

#define ODROID_DIALOG_CHOICE_LAST {0x0F0F0F0F, "LAST", (char *)"LAST", 0xFFFF, NULL}

int odroid_overlay_settings_menu(odroid_dialog_choice_t *extra_options, void_callback_t repaint, odroid_menu_flags_t flags);
void odroid_overlay_draw_fill_rect(int x, int y, int w, int h, uint16_t color);
void odroid_overlay_draw_text(int x, int y, int w, const char *text, uint16_t color, uint16_t bg);
int odroid_overlay_get_font_width(void);
#endif
