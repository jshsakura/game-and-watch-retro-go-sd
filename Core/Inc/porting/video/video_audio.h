// Chunk-fed MP3 audio for the Video player. Reuses the Music app's brick-safe
// recipe: an in-overlay PCM ring registered with the core SAI ISR via
// music_attach() (the ISR only READS the ring, never calls overlay code), fed by
// minimp3 + downmix-to-mono + nearest resample to 48 kHz. Unlike the Music
// engine (which streams from a file) this is fed the AVI's '01wb' chunks as the
// demuxer yields them.
#pragma once

#include <stdint.h>

void video_audio_start(void);                 // init + attach the ring to the ISR
void video_audio_feed(const uint8_t *mp3, int len);   // decode + enqueue MP3 bytes
int  video_audio_ring_count(void);            // queued 48 kHz samples (for back-pressure)
void video_audio_stop(void);                  // silence the ring
