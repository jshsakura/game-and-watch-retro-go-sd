#ifndef STUB_RG_RTC_H
#define STUB_RG_RTC_H
#include <stdint.h>
#include <time.h>
int GW_GetCurrentHour(void); int GW_GetCurrentMinute(void); int GW_GetCurrentSubSeconds(void);
int GW_GetCurrentMonth(void); int GW_GetCurrentDay(void); int GW_GetCurrentWeekday(void);
void GW_GetUnixTM(struct tm *tm);
void GW_SetUnixTM(struct tm *tm);
#endif
