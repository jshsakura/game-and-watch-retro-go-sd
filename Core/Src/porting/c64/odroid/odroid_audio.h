#pragma once
#include <stdint.h>

/* Audio abstraction the Frodo DigitalRenderer pushes SID samples through.
 * Implemented in main_c64_dev.cpp as a ring buffer that C64Display::Update()
 * drains one device-DMA frame at a time (push -> pull bridge). */
#ifdef __cplusplus
extern "C" {
#endif
void odroid_audio_init(int sample_rate);
void odroid_audio_submit(const int16_t *stereo, int frames);
void odroid_audio_terminate(void);
static inline void odroid_audio_submit_zero(void) {}
#ifdef __cplusplus
}
#endif
