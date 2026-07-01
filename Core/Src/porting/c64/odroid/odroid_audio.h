#pragma once
#include <stdint.h>

/* Audio abstraction the Frodo DigitalRenderer pushes SID samples through.
 * Named c64sid_* (NOT odroid_audio_submit — that's the G&W framework's own API,
 * reusing the name collides). Implemented in main_c64_dev.cpp as a ring buffer
 * that C64Display::Update() drains one device-DMA frame at a time (push->pull). */
#ifdef __cplusplus
extern "C" {
#endif
void c64sid_audio_init(int sample_rate);
void c64sid_audio_submit(const int16_t *stereo, int frames);
void c64sid_audio_terminate(void);
static inline void odroid_audio_submit_zero(void) {}
#ifdef __cplusplus
}
#endif
