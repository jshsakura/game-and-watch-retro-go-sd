#pragma once

#include "stdbool.h"
#include "stdint.h"
#include "rg_i18n_lang.h"

#define ODROID_DIALOG_CHOICE_SEPARATOR {0x0F0F0F0E, "-", "-", -1, NULL}

#define FONT_COUNT 9

extern const char* gui_fonts[];
extern const int gui_font_count;

void set_font(uint8_t font_index);
uint8_t get_font();

extern lang_t* curr_lang;
extern const lang_t* gui_lang[];
extern const int gui_lang_count;

/* Load a language's strings from /lang/xx_xx.bin (built by
 * tools/gen_i18n_bin.py). On success returns a pointer to a static
 * RAM-resident lang_t whose s_XXX fields point into a malloc'd buffer
 * holding the SD-loaded strings. On any error (file missing, bad
 * magic, OOM, etc.) returns the baked en_us fallback. Caller should
 * assign the result to curr_lang. Safe to call repeatedly; each call
 * realloc()s the strings buffer in place. */
lang_t *i18n_load_language(int idx);

int i18n_get_text_height();

int  i18n_get_text_width(const char *text);
int  i18n_get_text_lines(const char *text, const int fix_width);

int  i18n_draw_text_line(uint16_t x_pos, uint16_t y_pos, uint16_t width, const char *text, uint16_t color, uint16_t color_bg, char transparent);
int  i18n_draw_text(uint16_t x_pos, uint16_t y_pos, uint16_t width, uint16_t max_height, const char *text, uint16_t color, uint16_t color_bg, char transparent);

void odroid_overlay_clock(int x_pos, int y_pos);


bool odroid_button_turbos(void);

int8_t odroid_settings_theme_get();
void odroid_settings_theme_set(int8_t theme);

int8_t odroid_settings_colors_get();
void odroid_settings_colors_set(int8_t colors);
int8_t odroid_settings_turbo_buttons_get();
void odroid_settings_turbo_buttons_set(int8_t turbo_buttons);

int8_t odroid_settings_font_get();
void odroid_settings_font_set(int8_t font);


int8_t odroid_settings_lang_get();
int8_t odroid_settings_get_next_lang(uint8_t cur);
int8_t odroid_settings_get_prior_lang(uint8_t cur);
void odroid_settings_lang_set(int8_t lang);
