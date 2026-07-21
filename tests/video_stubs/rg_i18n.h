/* Host-test stub. Real rg_i18n.h pulls in the whole generated lang_t (200+
 * fields); video_play.c only reads s_info / s_Quit_to_menu through the VTR
 * fallback macro, so this lang_t carries just those two. */
#ifndef STUB_RG_I18N_H
#define STUB_RG_I18N_H
#include <stdint.h>

#define ODROID_DIALOG_CHOICE_SEPARATOR {0x0F0F0F0E, "-", "-", -1, NULL}

typedef struct { const char *s_info, *s_Quit_to_menu; } lang_t;
extern const lang_t *curr_lang;

int i18n_draw_text_line(uint16_t x, uint16_t y, uint16_t w, const char *t, uint16_t c, uint16_t bg, char f);
int i18n_get_text_width(const char *t);
#endif
