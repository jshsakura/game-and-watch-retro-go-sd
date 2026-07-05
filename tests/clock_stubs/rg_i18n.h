#ifndef STUB_RG_I18N_H
#define STUB_RG_I18N_H
#include <stdint.h>
typedef struct { const char *s_AM, *s_PM, *s_Clock,
  *s_Weekday_Mon,*s_Weekday_Tue,*s_Weekday_Wed,*s_Weekday_Thu,*s_Weekday_Fri,*s_Weekday_Sat,*s_Weekday_Sun; } lang_t;
extern const lang_t *curr_lang;
int i18n_draw_text_line(int x, int y, int w, const char *t, uint16_t c, uint16_t bg, int f);
#endif
