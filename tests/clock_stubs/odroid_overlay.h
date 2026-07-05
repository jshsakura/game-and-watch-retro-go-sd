#ifndef STUB_ODROID_OVERLAY_H
#define STUB_ODROID_OVERLAY_H
#include <stdint.h>
typedef enum { ODROID_DIALOG_INIT, ODROID_DIALOG_PREV, ODROID_DIALOG_NEXT, ODROID_DIALOG_ENTER, ODROID_DIALOG_FOCUS_GAINED } odroid_dialog_event_t;
typedef struct odroid_dialog_choice odroid_dialog_choice_t;
typedef bool (*odroid_dialog_cb)(odroid_dialog_choice_t *, odroid_dialog_event_t, uint32_t);
struct odroid_dialog_choice { int id; const char *label; char *value; int enabled; odroid_dialog_cb update_cb; };
#define ODROID_DIALOG_CHOICE_LAST {0x0F0F0F0F, "LAST", "LAST", 0xFFFF, NULL}
int odroid_overlay_dialog(const char *header, odroid_dialog_choice_t *options, int selected, void (*repaint)(void), int flags);
void odroid_overlay_draw_fill_rect(int x, int y, int w, int h, uint16_t color);
void odroid_overlay_draw_text(int x, int y, int w, const char *text, uint16_t color, uint16_t bg);
void odroid_overlay_draw_logo(int x, int y, int logo, uint16_t color);
void odroid_overlay_draw_battery(int pct, int x, int y);
int odroid_overlay_get_font_width(void);
#endif
