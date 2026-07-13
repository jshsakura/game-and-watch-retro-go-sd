/* Host-test stub for Core/Inc/retro-go/rg_alarm.h -- common.c only calls
 * rg_alarm_poll() once per frame from common_emu_input_loop(). The real
 * poll/tone logic is exercised by tests/test_alarm.c. */
#ifndef STUB_RG_ALARM_H
#define STUB_RG_ALARM_H
#include <stdbool.h>

bool rg_alarm_poll(void);

#endif
