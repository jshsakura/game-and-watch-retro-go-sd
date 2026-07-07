/* Host-test stub — mirrors Core/Inc/retro-go/rg_clock_alarm_mp3.h. The whole-file
 * rg_clock.c test stubs these out (like the GIF/album stubs); the module's own
 * logic is covered by tests/test_clock_mp3.c against the real .c. */
#pragma once
#include <stdbool.h>

bool clock_alarm_mp3_available(void);
bool clock_alarm_mp3_start(void);
void clock_alarm_mp3_service(int volume);
void clock_alarm_mp3_stop(void);
bool clock_alarm_mp3_active(void);
