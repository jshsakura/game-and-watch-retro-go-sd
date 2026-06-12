// Streaming MP3 audio engine for the Music app.
//
// minimp3 decode + downmix to mono + nearest-neighbour resample to 48 kHz,
// feeding a decoded-PCM ring buffer. The ring decouples the SD read/decode from
// the audio DMA deadline (prevents the periodic stutter). Also provides seek and
// duration estimation.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Pump target: keep the ring this full each iteration (ring is 8192, leave a
// frame of headroom).
#define AUDIO_PUMP_TARGET  (8192 - 1152)

// Open (or reopen) a track. Returns true on success. Computes the audio data
// offset (past any ID3v2 tag) and total size for seeking / duration.
bool audio_open(const char *path);
void audio_close(void);

void    audio_ring_reset(void);
void    audio_pump(int target);        // decode-ahead until ring >= target
int     audio_ring_count(void);
bool    audio_eof(void);               // decode reached end of stream

// Reposition playback to a fraction [0,1) of the audio data. Re-syncs the
// decoder, clears the ring and re-pumps.
void    audio_seek(float frac);

int  audio_duration_sec(void);         // estimate from last bitrate + size
int  audio_bitrate_kbps(void);
int  audio_src_hz(void);
int  audio_channels(void);

// Standalone first-frame duration estimate for list rows (does not disturb the
// currently-open track's decoder state beyond reinit; callers use it before
// opening a track for playback).
int  audio_quick_duration(const char *path);
