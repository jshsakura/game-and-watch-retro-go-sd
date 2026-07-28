// Resume positions for the video player. See video_resume.h for the contract.
//
// One line per clip, most recent first:
//
//     <frame>\t<path>\n
//
// Text, because it is a handful of short lines that a person may want to read or
// delete from a card reader, and because a binary struct here would be one more
// thing to version. Capped at RESUME_MAX entries, LRU by rewrite order, so the
// file cannot grow without bound on a card with a large library.
#include "video_resume.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Beside the saves, not at the root of the card: /data is where this firmware
// keeps everything it writes, and the root belongs to the person holding the SD.
/* Overridable so the host test can point it at /tmp. On the device it is always
 * this: /data is where the firmware keeps what it writes. */
#ifndef RESUME_PATH
#define RESUME_PATH   "/data/video_resume.txt"
#endif
#define RESUME_MAX    32
#define RESUME_TMP    RESUME_PATH ".new"
#define LINE_MAX      (8 + 1 + 255 + 2)

// Below this, restarting is friendlier than resuming: nobody wants "continue" to
// drop them four seconds in. At 30fps that is ten seconds.
#define RESUME_MIN_FRAMES   300
// And within this of the end the clip counts as finished, so it starts over next
// time instead of resuming into the last moments.
#define RESUME_END_FRAMES   150

/* Delete / atomic-rename. newlib rename() has no syscall on either build, so the
 * storage backend supplies them -- FatFs on SD builds, LittleFS on the flash
 * variant. Same split as rg_favorites.c, which solves the same problem. */
#if defined(RESUME_HOST_STDIO)
/* The host suite. Only the primitives differ; which lines survive and when the
 * store is deleted -- the part with the edges in it -- is the same code. */
static void resume_delete(const char *p) { remove(p); }
static bool resume_commit(const char *tmp, const char *dst)
{
    remove(dst);
    return rename(tmp, dst) == 0;
}
#elif !defined(SD_CARD) || SD_CARD == 1
#include "ff.h"
static void resume_delete(const char *p) { f_unlink(p); }
static bool resume_commit(const char *tmp, const char *dst)
{
    f_unlink(dst);                       /* f_rename needs the name free */
    return f_rename(tmp, dst) == FR_OK;
}
#else
#include "gw_littlefs.h"
static void resume_delete(const char *p) { fs_delete(p); }
static bool resume_commit(const char *tmp, const char *dst)
{
    fs_delete(dst);
    return fs_rename(tmp, dst) >= 0;
}
#endif

static bool line_split(char *line, int *frame, char **path)
{
    char *tab = strchr(line, '\t');
    if (!tab) return false;
    *tab = '\0';
    *frame = (int)strtol(line, NULL, 10);
    *path = tab + 1;
    char *nl = strchr(*path, '\n');
    if (nl) *nl = '\0';
    return **path != '\0' && *frame > 0;
}

int video_resume_get(const char *path)
{
    if (!path || !*path) return 0;

    FILE *f = fopen(RESUME_PATH, "r");
    if (!f) return 0;

    char line[LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        int frame;
        char *p;
        if (!line_split(line, &frame, &p)) continue;
        if (strcmp(p, path) == 0) { found = frame; break; }
    }
    fclose(f);
    return found >= RESUME_MIN_FRAMES ? found : 0;
}

void video_resume_put(const char *path, int frame, int total)
{
    if (!path || !*path) return;

    // Watched to the end (or near enough) -> forget it, so the next open starts
    // from the beginning rather than from the credits. Same for a position too
    // early to be worth restoring: writing it would only make the file longer.
    const bool finished = (total > 0 && frame >= total - RESUME_END_FRAMES);
    const bool keep = (frame >= RESUME_MIN_FRAMES) && !finished;

    // STREAMED through a temp file, not collected in an array first. The obvious
    // version keeps the surviving lines in a static char[32][266] and rewrites --
    // 8.5 KB of BSS, which is 8.5 KB this overlay does not have: the linker
    // answered with "MUSIC BSS overflow" and refused to link. This is the same
    // shape rg_favorites.c uses for the same reason, and it is also crash-safe --
    // the live file is only replaced once the new one is complete.
    FILE *out = fopen(RESUME_TMP, "w");
    if (!out) return;

    bool ok = true;
    int n = 0;
    if (keep) {
        ok = fprintf(out, "%d\t%s\n", frame, path) > 0;   // most recent first
        n++;
    }

    FILE *in = fopen(RESUME_PATH, "r");
    if (in) {
        char line[LINE_MAX];
        while (ok && n < RESUME_MAX && fgets(line, sizeof line, in)) {
            int old_frame;
            char *p;
            if (!line_split(line, &old_frame, &p)) continue;
            if (strcmp(p, path) == 0) continue;          // superseded by this write
            ok = fprintf(out, "%d\t%s\n", old_frame, p) > 0;
            n++;
        }
        fclose(in);
    }
    fclose(out);

    if (!ok) { resume_delete(RESUME_TMP); return; }

    // Nothing left to remember: delete the store rather than leaving the OLD file
    // on disk. Returning early here is what the host test caught -- finishing the
    // only clip in the store dropped its entry and never wrote, so the next open
    // resumed at the position that was supposed to have been erased.
    if (n == 0) {
        resume_delete(RESUME_TMP);
        resume_delete(RESUME_PATH);
        return;
    }
    resume_commit(RESUME_TMP, RESUME_PATH);
}
