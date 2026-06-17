// Host unit tests for browser/navigation logic from main_music.c.
// Duplicates the pure functions (static in the source) so any logic change
// must also update these tests (red-green safety net).
// Build+run: see tests/run.sh
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)
#define CHECK_STR(a, b, msg) do { checks++; if (strcmp((a), (b)) != 0) { \
    printf("FAIL: %s : got '%s' want '%s'\n", msg, (a), (b)); fails++; } } while (0)
#define CHECK_INT(a, b, msg) do { checks++; if ((a) != (b)) { printf("FAIL: %s: got %d want %d\n", msg, (int)(a), (int)(b)); fails++; } } while (0)

#define REPEAT_OFF 0
#define REPEAT_ALL 1
#define REPEAT_ONE 2
#define MAX_ENTRIES 384
#define VISIBLE_ROWS 5

#define NAME_MAX_LEN 128
#define PATH_MAX_LEN 256

// --- duplicated pure functions from main_music.c ---

static bool has_ext(const char *name, const char *ext)
{
    size_t ln = strlen(name), le = strlen(ext);
    if (ln < le) return false;
    const char *s = name + ln - le;
    for (size_t i = 0; i < le; i++) {
        char a = s[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static const char *strip_ext(const char *name, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", name);
    char *d = strrchr(buf, '.');
    if (d) *d = '\0';
    return buf;
}

// --- simulated browser state ---

typedef struct {
    char name[NAME_MAX_LEN];
    bool is_dir;
    bool is_special;
} test_entry_t;

static test_entry_t entries[MAX_ENTRIES];
static int entry_count, cursor, scroll;

static bool is_track(int i) { return !entries[i].is_dir && !entries[i].is_special; }

static int pl_count(void)
{
    int c = 0;
    for (int i = 0; i < entry_count; i++) if (is_track(i)) c++;
    return c;
}

static void move_cursor(int delta)
{
    if (entry_count == 0) return;
    cursor += delta;
    if (cursor < 0) cursor = 0;
    if (cursor >= entry_count) cursor = entry_count - 1;
    if (cursor < scroll) scroll = cursor;
    if (cursor >= scroll + VISIBLE_ROWS) scroll = cursor - VISIBLE_ROWS + 1;
}

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rng(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static int pick_next(int pi, bool shuffle, int repeat)
{
    int n = pl_count();
    if (n <= 0) return -1;
    if (repeat == REPEAT_ONE) return pi;
    if (shuffle) return (int)(rng() % (uint32_t)n);
    if (pi + 1 < n) return pi + 1;
    return (repeat == REPEAT_ALL) ? 0 : -1;
}

static int pick_prev(int pi, int repeat)
{
    int n = pl_count();
    if (n <= 0) return -1;
    if (pi - 1 >= 0) return pi - 1;
    return (repeat == REPEAT_ALL) ? n - 1 : 0;
}

static int cursor_to_pl(int cur)
{
    int pi = 0;
    for (int i = 0; i < cur; i++) if (is_track(i)) pi++;
    return pi;
}

static void reset_entries(int count)
{
    entry_count = count;
    cursor = 0;
    scroll = 0;
    for (int i = 0; i < count && i < MAX_ENTRIES; i++) {
        snprintf(entries[i].name, NAME_MAX_LEN, "track%03d.mp3", i);
        entries[i].is_dir = false;
        entries[i].is_special = false;
    }
}

// --- tests ---

static void test_has_ext_mp3(void)
{
    CHECK(has_ext("song.mp3", ".mp3"), "song.mp3 matches .mp3");
    CHECK(has_ext("song.MP3", ".mp3"), "song.MP3 matches .mp3 (case insensitive)");
    CHECK(has_ext("song.Mp3", ".MP3"), "mixed case match");
    CHECK(!has_ext("song.ogg", ".mp3"), "song.ogg does not match .mp3");
    CHECK(!has_ext("mp3", ".mp3"), "bare 'mp3' too short");
    CHECK(has_ext("my.song.mp3", ".mp3"), "double dot matches");
    CHECK(!has_ext("song.mp3.bak", ".mp3"), "wrong extension");
}

static void test_has_ext_edge(void)
{
    CHECK(has_ext(".mp3", ".mp3"), "hidden file .mp3 matches");
    CHECK(!has_ext("", ".mp3"), "empty string no match");
    CHECK(has_ext("a.mp3", ".mp3"), "single char name");
}

static void test_strip_ext(void)
{
    char buf[128];
    CHECK_STR(strip_ext("song.mp3", buf, sizeof(buf)), "song", "strip .mp3");
    CHECK_STR(strip_ext("my.song.flac", buf, sizeof(buf)), "my.song", "strip last ext only");
    CHECK_STR(strip_ext("noext", buf, sizeof(buf)), "noext", "no extension unchanged");
    CHECK_STR(strip_ext(".hidden", buf, sizeof(buf)), "", "hidden file strips to empty");
}

static void test_move_cursor_basic(void)
{
    reset_entries(10);
    cursor = 0; scroll = 0;
    move_cursor(1);
    CHECK_INT(cursor, 1, "move down 1");
    move_cursor(3);
    CHECK_INT(cursor, 4, "move down 3");
    move_cursor(-2);
    CHECK_INT(cursor, 2, "move up 2");
}

static void test_move_cursor_clamp(void)
{
    reset_entries(5);
    cursor = 0; scroll = 0;
    move_cursor(-1);
    CHECK_INT(cursor, 0, "clamped at top");
    cursor = 4;
    move_cursor(1);
    CHECK_INT(cursor, 4, "clamped at bottom");
}

static void test_move_cursor_scroll_follows(void)
{
    reset_entries(20);
    cursor = 0; scroll = 0;
    move_cursor(5);
    CHECK_INT(cursor, 5, "cursor at 5");
    CHECK_INT(scroll, 1, "scroll follows (cursor 5 - vis 5 + 1 = 1)");
    cursor = 0; scroll = 0;
    move_cursor(10);
    CHECK_INT(cursor, 10, "cursor at 10");
    CHECK(scroll <= 6, "scroll keeps cursor visible");
}

static void test_move_cursor_scroll_back(void)
{
    reset_entries(20);
    cursor = 19; scroll = 15;
    move_cursor(-10);
    CHECK_INT(cursor, 9, "cursor moved to 9");
    CHECK_INT(scroll, 9, "scroll = cursor when cursor < scroll");
}

static void test_move_cursor_page(void)
{
    reset_entries(30);
    cursor = 0; scroll = 0;
    move_cursor(VISIBLE_ROWS);
    CHECK_INT(cursor, VISIBLE_ROWS, "page down");
    move_cursor(-VISIBLE_ROWS);
    CHECK_INT(cursor, 0, "page up");
}

static void test_move_cursor_empty(void)
{
    entry_count = 0;
    cursor = 0; scroll = 0;
    move_cursor(1);
    CHECK_INT(cursor, 0, "no crash on empty");
}

static void test_pick_next_linear(void)
{
    reset_entries(10);
    CHECK_INT(pick_next(0, false, REPEAT_OFF), 1, "next 0->1");
    CHECK_INT(pick_next(4, false, REPEAT_OFF), 5, "next 4->5");
}

static void test_pick_next_repeat_off(void)
{
    reset_entries(5);
    CHECK_INT(pick_next(4, false, REPEAT_OFF), -1, "end of list, no repeat");
}

static void test_pick_next_repeat_all(void)
{
    reset_entries(5);
    CHECK_INT(pick_next(4, false, REPEAT_ALL), 0, "wrap around with repeat all");
}

static void test_pick_next_repeat_one(void)
{
    reset_entries(5);
    CHECK_INT(pick_next(2, false, REPEAT_ONE), 2, "repeat one stays");
}

static void test_pick_next_shuffle(void)
{
    reset_entries(100);
    g_rng = 12345;
    int n = pick_next(0, true, REPEAT_OFF);
    CHECK(n >= 0 && n < pl_count(), "shuffle returns valid index");
}

static void test_pick_prev_basic(void)
{
    reset_entries(10);
    CHECK_INT(pick_prev(5, REPEAT_OFF), 4, "prev 5->4");
    CHECK_INT(pick_prev(0, REPEAT_OFF), 0, "prev at 0 stays at 0 (no repeat)");
}

static void test_pick_prev_repeat_all(void)
{
    reset_entries(5);
    CHECK_INT(pick_prev(0, REPEAT_ALL), 4, "prev wraps with repeat all");
}

static void test_pl_count_with_dirs(void)
{
    reset_entries(6);
    entries[0].is_dir = true;
    entries[3].is_dir = true;
    CHECK_INT(pl_count(), 4, "only tracks counted (4 of 6)");
}

static void test_cursor_to_pl(void)
{
    reset_entries(6);
    entries[0].is_dir = true;
    entries[3].is_dir = true;
    // tracks at indices 1,2,4,5
    CHECK_INT(cursor_to_pl(1), 0, "first track -> pl 0");
    CHECK_INT(cursor_to_pl(2), 1, "second track -> pl 1");
    CHECK_INT(cursor_to_pl(4), 2, "third track -> pl 2");
    CHECK_INT(cursor_to_pl(5), 3, "fourth track -> pl 3");
}

static void test_cursor_to_pl_all_tracks(void)
{
    reset_entries(5);
    CHECK_INT(cursor_to_pl(0), 0, "all tracks: idx 0 -> pl 0");
    CHECK_INT(cursor_to_pl(4), 4, "all tracks: idx 4 -> pl 4");
}

static void test_scroll_never_negative(void)
{
    reset_entries(30);
    cursor = 29; scroll = 25;
    move_cursor(-30);
    CHECK_INT(scroll, 0, "scroll clamped at 0");
}

static void test_large_library(void)
{
    reset_entries(MAX_ENTRIES);
    CHECK_INT(entry_count, MAX_ENTRIES, "max entries loaded");
    cursor = 0; scroll = 0;
    move_cursor(MAX_ENTRIES - 1);
    CHECK_INT(cursor, MAX_ENTRIES - 1, "last entry reachable");
    CHECK(scroll + VISIBLE_ROWS > cursor, "last entry visible");
}

int main(void)
{
    test_has_ext_mp3();
    test_has_ext_edge();
    test_strip_ext();
    test_move_cursor_basic();
    test_move_cursor_clamp();
    test_move_cursor_scroll_follows();
    test_move_cursor_scroll_back();
    test_move_cursor_page();
    test_move_cursor_empty();
    test_pick_next_linear();
    test_pick_next_repeat_off();
    test_pick_next_repeat_all();
    test_pick_next_repeat_one();
    test_pick_next_shuffle();
    test_pick_prev_basic();
    test_pick_prev_repeat_all();
    test_pl_count_with_dirs();
    test_cursor_to_pl();
    test_cursor_to_pl_all_tracks();
    test_scroll_never_negative();
    test_large_library();
    printf("browser: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
