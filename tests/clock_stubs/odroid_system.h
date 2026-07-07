#ifndef STUB_ODROID_SYSTEM_H
#define STUB_ODROID_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define ODROID_AUDIO_VOLUME_MAX 9
int odroid_audio_volume_get(void);
void odroid_audio_volume_set(int level);
void odroid_system_sleep(void);
uint8_t odroid_display_get_backlight_raw(void);
#endif
