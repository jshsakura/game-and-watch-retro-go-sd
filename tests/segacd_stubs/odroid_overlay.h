#ifndef SEGACD_STUBS_OVERLAY_H
#define SEGACD_STUBS_OVERLAY_H
#include <stdint.h>
/* Signature mirrors retro-go-stm32/components/odroid/odroid_overlay.h:55. */
int odroid_overlay_draw_text(uint16_t x, uint16_t y, uint16_t width,
                             const char *text, uint16_t color,
                             uint16_t color_bg);
#endif
