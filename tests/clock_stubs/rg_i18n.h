#ifndef STUB_RG_I18N_H
#define STUB_RG_I18N_H
#include <stdint.h>
typedef struct { const char *s_AM, *s_PM, *s_Clock,
  *s_Weekday_Mon,*s_Weekday_Tue,*s_Weekday_Wed,*s_Weekday_Thu,*s_Weekday_Fri,*s_Weekday_Sat,*s_Weekday_Sun,
  *s_Clock_Pomodoro,*s_Clock_Timer,*s_Clock_Stopwatch,*s_Clock_Work,*s_Clock_Break,*s_Clock_Cycle,
  *s_Clock_Ringing,*s_Clock_Hint_Clock,*s_Clock_Hint_Run,*s_Clock_Hint_Stop,*s_Clock_Hint_TimerStop,
  *s_Clock_Hint_Editor,*s_Clock_Hint_Edit,*s_Clock_Add_Alarm,*s_Clock_Done,*s_Clock_On,*s_Clock_Off,
  *s_Clock_Format,*s_Clock_DND,*s_Clock_Anim,*s_Clock_Anim_0,*s_Clock_Anim_1,*s_Clock_Anim_2,
  *s_Clock_Volume,*s_Clock_Alarms,*s_Clock_Exit,*s_Clock_Hint_Ring,*s_Clock_Theme,*s_Clock_Face,*s_Clock_Auto,*s_Clock_Anim_3,*s_Full,*s_Fill; } lang_t;
extern const lang_t *curr_lang;
int i18n_draw_text_line(int x, int y, int w, const char *t, uint16_t c, uint16_t bg, int f);
int i18n_get_text_width(const char *t);
#endif
