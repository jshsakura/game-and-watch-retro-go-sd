/* Resume positions for the video player -- the real video_resume.c, on the host.
 *
 * This is pure FILE* logic, which is the whole reason it is testable: the rest of
 * the player needs a JPEG peripheral, a SAI clock and an SD card. What it has to
 * get right is entirely about edges -- a clip watched to the end, one stopped two
 * seconds in, a store that must not grow without bound, and a rewrite that must
 * not lose the other entries -- and every one of those is a data case.
 *
 * RESUME_PATH is overridden at compile time so the test writes to /tmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_resume.h"

#ifndef TEST_RESUME_PATH
#define TEST_RESUME_PATH "/tmp/mtest/video_resume.txt"
#endif

static int rc = 0;

static void ok(const char *m) { printf("  OK   %s\n", m); }
static void bad(const char *m) { printf("  FAIL %s\n", m); rc = 1; }

static void wipe(void) { remove(TEST_RESUME_PATH); }

static int lines(void)
{
    FILE *f = fopen(TEST_RESUME_PATH, "r");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int main(void)
{
    printf("=== video: resume positions survive the edges ===\n");
    wipe();

    /* 1. Unknown clip: start from the beginning, and do not create a file just
     *    for having been asked. */
    if (video_resume_get("/media/a.avi") != 0) bad("an unknown clip did not answer 0");
    else if (lines() != -1)                    bad("asking about an unknown clip created the store");
    else ok("an unknown clip resumes at 0 and writes nothing");

    /* 2. A real position round-trips. */
    video_resume_put("/media/a.avi", 5000, 20000);
    if (video_resume_get("/media/a.avi") != 5000) bad("a stored position did not come back");
    else ok("a position round-trips");

    /* 3. Too early to be worth restoring. Resuming four seconds in is worse than
     *    starting over, and storing it would only lengthen the file. */
    wipe();
    video_resume_put("/media/b.avi", 60, 20000);
    if (video_resume_get("/media/b.avi") != 0) bad("a position a couple of seconds in was restored");
    else if (lines() != -1)                    bad("a too-early position was written anyway");
    else ok("a position too near the start is ignored, not stored");

    /* 4. Watched to the end ERASES the entry. Storing an end-of-file position
     *    would make the next open resume into the credits -- which looks exactly
     *    like the clip refusing to play. */
    wipe();
    video_resume_put("/media/c.avi", 5000, 20000);
    video_resume_put("/media/c.avi", 19990, 20000);
    if (video_resume_get("/media/c.avi") != 0) bad("finishing a clip left a resume position");
    else ok("finishing a clip clears its position");

    /* 5. A rewrite must not lose the other clips. This is the one that a naive
     *    "open, write my line, close" gets wrong. */
    wipe();
    video_resume_put("/media/x.avi", 1000, 99999);
    video_resume_put("/media/y.avi", 2000, 99999);
    video_resume_put("/media/z.avi", 3000, 99999);
    if (video_resume_get("/media/x.avi") != 1000 ||
        video_resume_get("/media/y.avi") != 2000 ||
        video_resume_get("/media/z.avi") != 3000)
        bad("storing one clip's position lost another's");
    else ok("three clips keep their own positions");

    /* 6. And updating one must replace it, not append a second line for it. */
    video_resume_put("/media/y.avi", 7777, 99999);
    if (video_resume_get("/media/y.avi") != 7777) bad("an updated position did not take");
    else if (lines() != 3)                        bad("updating a clip appended instead of replacing");
    else ok("updating a clip replaces its line");

    /* 7. The store is capped. A large library must not grow the file for ever. */
    wipe();
    for (int i = 0; i < 100; i++) {
        char p[64];
        snprintf(p, sizeof p, "/media/clip%03d.avi", i);
        video_resume_put(p, 1000 + i, 99999);
    }
    int n = lines();
    if (n < 1 || n > 32) {
        char m[80];
        snprintf(m, sizeof m, "the store grew to %d lines (cap is 32)", n);
        bad(m);
    } else {
        ok("the store stays within its cap");
    }
    /* ...and the most recent write is the one that survives. */
    if (video_resume_get("/media/clip099.avi") != 1099)
        bad("the most recent clip was evicted by the cap");
    else
        ok("the most recent clip survives eviction");

    wipe();
    printf("\n%s\n", rc ? "FAILED" : "PASSED");
    return rc;
}
