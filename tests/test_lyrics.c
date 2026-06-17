// Host unit tests for music_lyrics (pure LRC/USLT parser).
// Build+run: see tests/run.sh
#include "music_lyrics.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define CHECK_STR(a, b, msg) do { checks++; if (strcmp((a), (b)) != 0) { \
    printf("FAIL: %s : got '%s' want '%s'\n", msg, (a), (b)); fails++; } } while (0)

static void test_synced_lrc(void)
{
    char buf[] = "[00:01.50]first line\r\n[00:03.00]second\n[01:05.20]third\n";
    lyrics_t ly;
    lyrics_parse(buf, &ly);

    CHECK(ly.synced, "lrc should be synced");
    CHECK(ly.n == 3, "lrc should have 3 lines");
    CHECK_STR(ly.line[0], "first line", "line0 text (CR stripped, tag stripped)");
    CHECK_STR(ly.line[1], "second", "line1 text");
    CHECK_STR(ly.line[2], "third", "line2 text");
    CHECK(ly.time_ms[0] == 1500, "line0 time 1.50s");
    CHECK(ly.time_ms[1] == 3000, "line1 time 3.00s");
    CHECK(ly.time_ms[2] == 65200, "line2 time 1:05.20");

    CHECK(lyrics_active_line(&ly, 0) == -1, "before first ts -> none");
    CHECK(lyrics_active_line(&ly, 1500) == 0, "exactly at first ts -> line0");
    CHECK(lyrics_active_line(&ly, 2999) == 0, "still line0");
    CHECK(lyrics_active_line(&ly, 3000) == 1, "line1");
    CHECK(lyrics_active_line(&ly, 999999) == 2, "past end -> last line");
}

static void test_multi_tag_line(void)
{
    // one line shared by two timestamps -> first timestamp wins
    char buf[] = "[00:10.00][00:20.00]chorus\n";
    lyrics_t ly;
    lyrics_parse(buf, &ly);
    CHECK(ly.synced, "multi-tag synced");
    CHECK(ly.n == 1, "multi-tag single line");
    CHECK(ly.time_ms[0] == 10000, "multi-tag first ts wins");
    CHECK_STR(ly.line[0], "chorus", "multi-tag text after both tags");
}

static void test_plain_unsynced(void)
{
    char buf[] = "Just some lyrics\nwithout timestamps\n";
    lyrics_t ly;
    lyrics_parse(buf, &ly);
    CHECK(!ly.synced, "plain text not synced");
    CHECK(ly.n == 2, "plain text 2 lines");
    CHECK_STR(ly.line[0], "Just some lyrics", "plain line0");
    CHECK(lyrics_active_line(&ly, 5000) == -1, "unsynced -> no active line");
}

static void test_empty(void)
{
    char buf[] = "";
    lyrics_t ly;
    lyrics_parse(buf, &ly);
    CHECK(ly.n == 0, "empty -> 0 lines");
    CHECK(!ly.synced, "empty -> not synced");
}

int main(void)
{
    test_synced_lrc();
    test_multi_tag_line();
    test_plain_unsynced();
    test_empty();
    printf("music_lyrics: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
