// Lyrics model + parser for the Music app (pure logic, host-testable).
//
// Parses LRC ("[mm:ss.xx] text", possibly multiple tags per line) and plain
// USLT lyrics. Detects timestamps -> synced playback highlight.
#pragma once

#include <stdbool.h>

#define LRC_MAX_LINES 240

typedef struct {
    int   n;
    int   time_ms[LRC_MAX_LINES];   // -1 when a line has no timestamp
    char *line[LRC_MAX_LINES];      // into the caller's backing buffer
    bool  synced;
} lyrics_t;

// Parse lyrics in place (mutates buf: inserts NULs, strips [..] tags).
void lyrics_parse(char *buf, lyrics_t *out);

// Index of the active line for play position `ms` (synced lyrics), else -1.
int  lyrics_active_line(const lyrics_t *ly, int ms);
