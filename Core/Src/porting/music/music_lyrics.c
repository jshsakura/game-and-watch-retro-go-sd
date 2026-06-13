// Lyrics parser — see music_lyrics.h.

#include "music_lyrics.h"
#include <stdio.h>
#include <string.h>

// "[mm:ss.xx]" or "[mm:ss]" -> milliseconds, or -1 if not a timestamp.
static int parse_ts(const char *s)
{
    int mm, ss, cs = 0;
    if (sscanf(s, "[%d:%d.%d]", &mm, &ss, &cs) >= 2)
        return mm * 60000 + ss * 1000 + cs * 10;
    if (sscanf(s, "[%d:%d]", &mm, &ss) == 2)
        return mm * 60000 + ss * 1000;
    return -1;
}

void lyrics_parse(char *buf, lyrics_t *out)
{
    out->n = 0;
    out->synced = false;
    if (!buf) return;

    char *p = buf;
    while (*p && out->n < LRC_MAX_LINES) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        size_t ln = strlen(p);
        if (ln && p[ln - 1] == '\r') p[ln - 1] = '\0';

        int ms = -1;
        char *text = p;
        while (*text == '[') {                 // consume leading [..] tags
            int t = parse_ts(text);
            char *close = strchr(text, ']');
            if (!close) break;
            if (t >= 0) { if (ms < 0) ms = t; out->synced = true; }
            text = close + 1;
        }
        while (*text == ' ') text++;

        out->time_ms[out->n] = ms;
        out->line[out->n] = text;
        out->n++;
        if (!eol) break;
        p = eol + 1;
    }
}

int lyrics_active_line(const lyrics_t *ly, int ms)
{
    if (!ly->synced) return -1;
    int act = -1;
    for (int i = 0; i < ly->n; i++)
        if (ly->time_ms[i] >= 0 && ly->time_ms[i] <= ms) act = i;
    return act;
}
