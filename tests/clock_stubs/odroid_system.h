#ifndef STUB_ODROID_SYSTEM_H
#define STUB_ODROID_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define ODROID_AUDIO_VOLUME_MAX 9
#define ODROID_BACKLIGHT_LEVEL_COUNT 10
int odroid_audio_volume_get(void);
void odroid_audio_volume_set(int level);
void odroid_system_sleep(void);
uint8_t odroid_display_get_backlight_raw(void);
int odroid_display_get_backlight(void);
void odroid_display_set_backlight(int level);
/* the one global "Idle power off" rule every idle-capable loop asks —
 * see odroid_system.h's real declaration; each test defines its own so it can
 * drive the states it cares about (same pattern as bq24072_get_state used to be). */
bool odroid_idle_timeout_expired(uint32_t idle_seconds);
#endif
