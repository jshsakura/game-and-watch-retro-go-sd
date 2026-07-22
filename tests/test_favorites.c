/* Host unit tests for Core/Src/retro-go/rg_favorites.c (the SD-file
 * favorites: /favorites.txt add/contains/remove/reset).
 *
 * rg_favorites.c is compiled as its own translation unit (NOT #included)
 * and linked with this file. FAVORITES_FILE / FAVORITES_TMP are redirected
 * into a /tmp scratch dir via -D on the command line (the production file
 * now #ifndef-guards those two #defines for exactly this purpose — see
 * Core/Src/retro-go/rg_favorites.c). Every other header rg_favorites.c
 * includes (odroid_system.h, rg_emulators.h, rg_utils.h, favorites.h,
 * rg_storage.h, rg_i18n.h, bitmaps.h, gui.h, ff.h) is stubbed under
 * tests/fav_stubs/ so this test needs no FatFs/GUI/emulator-list code —
 * only the four public file-IO entry points are exercised.
 *
 * Compile + run:
 *   gcc -O2 -Wall -Wextra -std=gnu11 -Itests/fav_stubs \
 *       -DFAVORITES_FILE='"/tmp/favtest/favorites.txt"' \
 *       -DFAVORITES_TMP='"/tmp/favtest/favorites.new"' \
 *       tests/test_favorites.c Core/Src/retro-go/rg_favorites.c \
 *       -o /tmp/mtest/test_favorites
 *   mkdir -p /tmp/favtest && /tmp/mtest/test_favorites
 *
 * (See tests/test_favorites.build for the same commands, one per line.)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "odroid_system.h"
#include "favorites.h"
#include "rg_emulators.h"
#include "rg_storage.h"
#include "rg_i18n.h"
#include "gui.h"

#ifndef FAVORITES_FILE
#define FAVORITES_FILE "/tmp/favtest/favorites.txt"
#endif
#ifndef FAVORITES_TMP
#define FAVORITES_TMP  "/tmp/favtest/favorites.new"
#endif

/* ---- link stubs for the gui/emulator-list code paths this test never
 * calls (favorites_refresh_tab, favorites_event_handler, register_tab) —
 * still referenced by rg_favorites.c's object code, so the linker needs
 * definitions even though none of these run in these tests. ------------- */
void *rg_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
retro_emulator_file_t *rg_emulators_shared_file_buffer(int *maxcount)
{ if (maxcount) *maxcount = 0; return NULL; }
const rom_system_t *rg_emulators_system_for_dir(const char *dirname, size_t len)
{ (void)dirname; (void)len; return NULL; }
bool emulator_show_file_menu(retro_emulator_file_t *file) { (void)file; return false; }
void emulator_show_file_info(retro_emulator_file_t *file) { (void)file; }
const char *rg_basename(const char *path)
{ const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
rg_stat_t rg_storage_stat(const char *path) { (void)path; rg_stat_t st = {0}; return st; }
static lang_t stub_lang = { .s_favorite = "Favorites", .s_no_favorite = "No favorites" };
lang_t *curr_lang = &stub_lang;   /* type must match Core/Inc/retro-go/rg_i18n.h */
tab_t *gui_add_tab(const char *name, int16_t logo_idx, int16_t header_idx, void *arg, void *event_handler)
{ (void)name; (void)logo_idx; (void)header_idx; (void)arg; (void)event_handler; return NULL; }
tab_t *gui_get_current_tab(void) { return NULL; }
listbox_item_t *gui_get_selected_item(tab_t *tab) { (void)tab; return NULL; }
void gui_resize_list(tab_t *tab, int new_size) { (void)tab; (void)new_size; }
bool gui_change_tab(int direction) { (void)direction; return false; }
uint8_t odroid_settings_SortMode_get(void) { return ODROID_SORT_NAME; }

/* ---- test fixture helpers ----------------------------------------------- */
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

/** Wipe both favorites files between tests so none leak state. */
static void reset_fixture(void)
{
    remove(FAVORITES_FILE);
    remove(FAVORITES_TMP);
}

/** Raw (non-API) write of the favorites file, for crafting edge-case
 * fixtures (blank lines, no trailing newline, hand-built duplicates). */
static void write_raw(const char *content)
{
    FILE *f = fopen(FAVORITES_FILE, "w");
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/** Number of raw lines in the favorites file (-1 if it doesn't exist). */
static int count_raw_lines(void)
{
    FILE *f = fopen(FAVORITES_FILE, "r");
    if (f == NULL)
        return -1;
    int n = 0;
    char buf[512];
    while (fgets(buf, sizeof buf, f))
        n++;
    fclose(f);
    return n;
}

/** True if `needle` appears as an exact (CR/LF-stripped) raw line. */
static bool raw_has_line(const char *needle)
{
    FILE *f = fopen(FAVORITES_FILE, "r");
    if (f == NULL)
        return false;
    char buf[512];
    bool found = false;
    while (!found && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        found = (strcmp(buf, needle) == 0);
    }
    fclose(f);
    return found;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/** Fill buf[0..total_len) with a repeating a..z pattern + NUL — a
 * deterministic path-shaped string of an exact length, for boundary tests. */
static void make_path(char *buf, size_t total_len)
{
    for (size_t i = 0; i < total_len; i++)
        buf[i] = (char)('a' + (i % 26));
    buf[total_len] = '\0';
}

/* ---- tests --------------------------------------------------------------- */

static void test_add_then_list(void)
{
    reset_fixture();
    CHECK(rg_favorites_add("/roms/nes/mario.nes") == true, "add first favorite succeeds");
    CHECK(rg_favorites_add("/roms/snes/zelda.sfc") == true, "add second favorite succeeds");
    CHECK(rg_favorites_contains("/roms/nes/mario.nes") == true, "first favorite is listed");
    CHECK(rg_favorites_contains("/roms/snes/zelda.sfc") == true, "second favorite is listed");
    CHECK(rg_favorites_contains("/roms/nes/luigi.nes") == false, "unrelated path not listed");
    CHECK(count_raw_lines() == 2, "file has exactly the two appended lines");
}

static void test_duplicate_add_is_noop(void)
{
    reset_fixture();
    CHECK(rg_favorites_add("/roms/nes/mario.nes") == true, "first add succeeds");
    CHECK(rg_favorites_add("/roms/nes/mario.nes") == true, "duplicate add still reports success");
    CHECK(count_raw_lines() == 1, "duplicate add does not append a second line");
    CHECK(rg_favorites_contains("/roms/nes/mario.nes") == true, "entry still listed once");
}

static void test_remove_middle_keeps_others(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    rg_favorites_add("/roms/nes/b.nes");
    rg_favorites_add("/roms/nes/c.nes");
    CHECK(rg_favorites_remove("/roms/nes/b.nes") == true, "remove of middle entry reports success");
    CHECK(rg_favorites_contains("/roms/nes/a.nes") == true, "entry before the removed one survives");
    CHECK(rg_favorites_contains("/roms/nes/c.nes") == true, "entry after the removed one survives");
    CHECK(rg_favorites_contains("/roms/nes/b.nes") == false, "removed entry is gone");
    CHECK(count_raw_lines() == 2, "file shrinks by exactly one line");
}

static void test_remove_nonexistent_path_is_noop(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    rg_favorites_add("/roms/nes/b.nes");
    CHECK(rg_favorites_remove("/roms/nes/not-there.nes") == true,
          "removing an absent path still reports success (rewrite of unchanged content)");
    CHECK(rg_favorites_contains("/roms/nes/a.nes") == true, "existing entry a survives");
    CHECK(rg_favorites_contains("/roms/nes/b.nes") == true, "existing entry b survives");
    CHECK(count_raw_lines() == 2, "no line dropped");
}

static void test_remove_when_file_absent(void)
{
    reset_fixture();
    CHECK(file_exists(FAVORITES_FILE) == false, "fixture starts with no favorites file");
    CHECK(rg_favorites_remove("/roms/nes/mario.nes") == false,
          "remove on a fresh boot (no favorites file yet) reports failure, not a crash");
}

static void test_contains_on_missing_file(void)
{
    reset_fixture();
    CHECK(rg_favorites_contains("/roms/nes/mario.nes") == false,
          "contains() on a fresh boot (no file) is false, not a crash");
}

static void test_reset_deletes_file(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    CHECK(file_exists(FAVORITES_FILE) == true, "file exists before reset");
    CHECK(rg_favorites_reset() == true, "reset reports success");
    CHECK(file_exists(FAVORITES_FILE) == false, "file removed by reset");
    CHECK(rg_favorites_contains("/roms/nes/a.nes") == false, "nothing favorited after reset");
}

static void test_reset_when_file_already_absent(void)
{
    reset_fixture();
    CHECK(file_exists(FAVORITES_FILE) == false, "fixture starts clean");
    CHECK(rg_favorites_reset() == true,
          "reset on an already-clean SD (no file) still reports success (FR_NO_FILE tolerated)");
}

static void test_blank_lines_and_no_trailing_newline(void)
{
    reset_fixture();
    /* Hand-craft a file with a leading blank line, a middle blank line, and
     * no trailing newline on the last entry — the kind of thing a partial
     * write or manual edit could leave behind. */
    write_raw("\n/roms/nes/a.nes\n\n/roms/nes/b.nes\n/roms/nes/c.nes");

    CHECK(rg_favorites_contains("/roms/nes/a.nes") == true, "entry after leading blank line is found");
    CHECK(rg_favorites_contains("/roms/nes/b.nes") == true, "entry after a mid-file blank line is found");
    CHECK(rg_favorites_contains("/roms/nes/c.nes") == true,
          "final entry with no trailing newline is still found");

    /* Removing an unrelated path forces a full rewrite; blank lines are
     * dropped by the parser's `line[0] == '\0'` skip regardless of which
     * path was targeted. */
    CHECK(rg_favorites_remove("/roms/nes/does-not-exist.nes") == true, "rewrite triggered by a no-op remove");
    CHECK(count_raw_lines() == 3, "blank lines were dropped by the rewrite");
    CHECK(raw_has_line("/roms/nes/a.nes") && raw_has_line("/roms/nes/b.nes") && raw_has_line("/roms/nes/c.nes"),
          "all three real entries preserved across the rewrite");
}

static void test_remove_drops_all_duplicate_lines(void)
{
    reset_fixture();
    /* rg_favorites_add() de-dupes, but a hand-edited or corrupted file could
     * still contain the same path twice — remove() should drop every
     * matching line, not just the first. */
    write_raw("/roms/nes/a.nes\n/roms/nes/dup.nes\n/roms/nes/dup.nes\n/roms/nes/b.nes\n");
    CHECK(count_raw_lines() == 4, "fixture has the duplicate line twice");
    CHECK(rg_favorites_remove("/roms/nes/dup.nes") == true, "remove of a duplicated path succeeds");
    CHECK(rg_favorites_contains("/roms/nes/dup.nes") == false, "duplicated path fully removed");
    CHECK(count_raw_lines() == 2, "both duplicate lines dropped, only a/b remain");
}

static void test_long_path_near_rg_path_max_boundary(void)
{
    reset_fixture();
    char just_under[RG_PATH_MAX];      /* RG_PATH_MAX - 1 chars, safely short of the buffer edge */
    char at_max[RG_PATH_MAX + 1];      /* exactly RG_PATH_MAX chars: line[] holds RG_PATH_MAX+1 bytes */
    make_path(just_under, RG_PATH_MAX - 1);
    make_path(at_max, RG_PATH_MAX);

    CHECK(rg_favorites_add(just_under) == true, "add path of RG_PATH_MAX-1 chars succeeds");
    CHECK(rg_favorites_contains(just_under) == true, "RG_PATH_MAX-1 path round-trips through add+contains");

    CHECK(rg_favorites_add(at_max) == true, "add path of exactly RG_PATH_MAX chars succeeds");
    CHECK(rg_favorites_contains(at_max) == true, "RG_PATH_MAX path round-trips through add+contains");
    /* A path this long leaves fgets's line buffer exactly full without
     * capturing the trailing '\n' on that read; the entry after it must
     * still be reachable on the next read_favorite_line() call. */
    CHECK(rg_favorites_add("/roms/nes/after-max.nes") == true, "add after a max-length line succeeds");
    CHECK(rg_favorites_contains("/roms/nes/after-max.nes") == true,
          "entry following a RG_PATH_MAX-length line is still found");

    CHECK(rg_favorites_remove(at_max) == true, "remove of the max-length path succeeds");
    CHECK(rg_favorites_contains(at_max) == false, "max-length path is gone after remove");
    CHECK(rg_favorites_contains(just_under) == true, "other entries unaffected by removing the max-length one");
    CHECK(rg_favorites_contains("/roms/nes/after-max.nes") == true, "trailing entry unaffected");
}

static void test_many_entries_no_artificial_cap(void)
{
    /* rg_favorites.c has no MAX_FAVORITES constant at the file-IO layer —
     * add/contains/remove operate on however many lines /favorites.txt
     * holds (any cap only exists later, in the tab's shared-buffer render
     * path, which this host test does not exercise). Confirm a healthy
     * batch round-trips with no hidden ceiling. */
    reset_fixture();
    char path[64];
    const int n = 200;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "/roms/nes/game%03d.nes", i);
        rg_favorites_add(path);
    }
    CHECK(count_raw_lines() == n, "all 200 entries were appended, no silent cap");

    bool all_found = true;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "/roms/nes/game%03d.nes", i);
        if (!rg_favorites_contains(path)) { all_found = false; break; }
    }
    CHECK(all_found, "every one of the 200 entries is still findable");

    CHECK(rg_favorites_remove("/roms/nes/game100.nes") == true, "remove works inside a large file");
    CHECK(rg_favorites_contains("/roms/nes/game100.nes") == false, "removed entry gone from a large file");
    CHECK(count_raw_lines() == n - 1, "large file shrinks by exactly one line");
}

int main(void)
{
    if (mkdir("/tmp/favtest", 0755) != 0 && errno != EEXIST) {
        perror("mkdir /tmp/favtest");
        return 1;
    }

    test_add_then_list();
    test_duplicate_add_is_noop();
    test_remove_middle_keeps_others();
    test_remove_nonexistent_path_is_noop();
    test_remove_when_file_absent();
    test_contains_on_missing_file();
    test_reset_deletes_file();
    test_reset_when_file_already_absent();
    test_blank_lines_and_no_trailing_newline();
    test_remove_drops_all_duplicate_lines();
    test_long_path_near_rg_path_max_boundary();
    test_many_entries_no_artificial_cap();

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
