#pragma once
#include <stdint.h>
int i18n_get_text_width(const char *t);
int i18n_get_text_height(void);
int i18n_draw_text_line(uint16_t x, uint16_t y, uint16_t w, const char *t, uint16_t fg, uint16_t bg, char transparent);
int i18n_draw_text(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *t, uint16_t fg, uint16_t bg, char transparent);
