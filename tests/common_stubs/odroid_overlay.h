/* Host-test stub, mirrors retro-go-stm32/components/odroid/odroid_overlay.h
 * (upstream-pinned submodule) just enough for common.c's menu/pause-banner
 * call sites to typecheck. Bodies are provided by the test .c. */
#ifndef STUB_ODROID_OVERLAY_H
#define STUB_ODROID_OVERLAY_H
#include <stdint.h>
#include <stdbool.h>
#include "odroid_input.h"

typedef enum {
    ODROID_MENU_FLAG_DRAW_ONLY = 1 << 0,
    ODROID_MENU_FLAG_NO_BG_DARKEN = 1 << 1,
} odroid_menu_flags_t;

typedef void (*void_callback_t)();
typedef int (*pause_input_callback_t)(odroid_gamepad_state_t *joystick);

typedef struct odroid_dialog_choice odroid_dialog_choice_t;

void odroid_overlay_sleep_pause_banner(void_callback_t repaint, odroid_menu_flags_t flags, pause_input_callback_t input_cb);
int  odroid_overlay_game_menu(odroid_dialog_choice_t *extra_options, void_callback_t repaint, odroid_menu_flags_t flags);
void odroid_overlay_draw_battery(odroid_battery_state_t battery, int x, int y);

#endif
