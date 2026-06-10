// ID3v2 metadata reader for the Music app.
//
// One-pass tag reader that pulls the useful text frames, embedded cover art and
// lyrics out of an MP3 and normalises every string to UTF-8 (so CJK renders via
// the i18n font). Supports ID3v2.2 (3-char frame ids) and v2.3 / v2.4.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ID3_FIELD       96      // bytes per short text field (UTF-8, NUL-term)
#define ID3_LYRICS_MAX  4096    // bytes of lyrics text (USLT or sidecar .lrc)

typedef struct {
    char title[ID3_FIELD];
    char artist[ID3_FIELD];
    char album[ID3_FIELD];
    char album_artist[ID3_FIELD];
    char composer[ID3_FIELD];
    char genre[ID3_FIELD];
    char year[16];
    char track[16];
    char comment[ID3_FIELD];
    char lyrics[ID3_LYRICS_MAX];   // empty unless has_lyrics
    bool has_lyrics;
    bool found;                    // an ID3v2 tag was present
} media_tags_t;

// Parse ID3v2 text frames + USLT lyrics into *out (all UTF-8). Missing fields
// become empty strings. Always clears *out first.
void id3_read_tags(const char *path, media_tags_t *out);

// Locate the first APIC (embedded cover) and read up to cap bytes into dst.
// Returns the image byte count (0 if none); sets *is_png (else assume JPEG).
int  id3_read_cover(const char *path, uint8_t *dst, int cap, bool *is_png);

// Load a sidecar "<track>.lrc" (replacing the .mp3 extension) into out as UTF-8.
// Returns true and sets *out NUL-terminated if found. EUC-KR bytes are passed
// through untouched (already-UTF-8 or ASCII files render correctly).
bool id3_read_lrc(const char *mp3path, char *out, int cap);
