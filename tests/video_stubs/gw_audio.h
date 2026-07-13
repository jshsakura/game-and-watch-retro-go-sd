/* Host-test stub. Trimmed COPY of Core/Inc/gw_audio.h's API surface — only
 * what video_audio.c and video_play.c actually call. Keep in sync with the
 * real header if the audio API changes; this is a separate copy per
 * tests/video_stubs (not shared with tests/clock_stubs) so the two test
 * scopes can't silently drift into each other. */
#ifndef _GW_AUDIO_H_
#define _GW_AUDIO_H_

#include "main.h"

#define AUDIO_SAMPLE_RATE   (48000)
#define AUDIO_BUFFER_LENGTH (1077)

void     music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail);
void     music_audio_enable(int on);
void     music_audio_set(int vol, int play);

void audio_start_playing(uint16_t length);
void audio_stop_playing(void);

#endif
