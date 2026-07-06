/* Host unit tests for Core/Src/retro-go/rg_favorites.c compiled with
 * -DSD_CARD=0 — i.e. the LittleFS (flash) build of the favorites file IO.
 *
 * The only behavioural difference from the SD1 build is rg_favorites_remove():
 * LittleFS allows a single open file, so the in+out temp-file rewrite is
 * replaced by a read-whole-file / filter-in-RAM / rewrite-in-place path (the
 * #else branch of rg_favorites_remove). fav_delete()/rg_favorites_reset() also
 * route through gw_littlefs's fs_delete() instead of FatFs f_unlink().
 *
 * Same TU-compiled-and-linked pattern as test_favorites.c. gw_littlefs.h is
 * supplied under tests/fav_stubs/ (declares fs_delete + LFS_ERR_NOENT); the
 * fs_delete() body lives here as a POSIX remove() wrapper.
 *
 * Compile + run (also in tests/test_favorites_sd0.build):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=0 -Itests/fav_stubs \
 *       -DFAVORITES_FILE='"/tmp/favtest0/favorites.txt"' \
 *       -DFAVORITES_TMP='"/tmp/favtest0/favorites.new"' \
 *       tests/test_favorites_sd0.c Core/Src/retro-go/rg_favorites.c \
 *       -o /tmp/mtest/test_favorites_sd0
 *   mkdir -p /tmp/favtest0 && /tmp/mtest/test_favorites_sd0
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
#include "gw_littlefs.h"

#ifndef FAVORITES_FILE
#define FAVORITES_FILE "/tmp/favtest0/favorites.txt"
#endif
#ifndef FAVORITES_TMP
#define FAVORITES_TMP  "/tmp/favtest0/favorites.new"
#endif

/* ---- gw_littlefs fs_delete(): POSIX remove() wrapper, mapping a missing
 * file to LFS_ERR_NOENT so rg_favorites_reset() tolerates it (mirrors the
 * FatFs FR_NO_FILE tolerance on the SD build). ---------------------------- */
int fs_delete(const char *path)
{
    if (remove(path) == 0)
        return 0;
    if (errno == ENOENT)
        return LFS_ERR_NOENT;
    return -1;
}

/* ---- link stubs for the gui/emulator-list code paths this test never calls
 * (same set as test_favorites.c). ---------------------------------------- */
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
static const lang_t stub_lang = { .s_favorite = "Favorites", .s_no_favorite = "No favorites" };
const lang_t *curr_lang = &stub_lang;
tab_t *gui_add_tab(const char *name, int16_t logo_idx, int16_t header_idx, void *arg, void *event_handler)
{ (void)name; (void)logo_idx; (void)header_idx; (void)arg; (void)event_handler; return NULL; }
tab_t *gui_get_current_tab(void) { return NULL; }
listbox_item_t *gui_get_selected_item(tab_t *tab) { (void)tab; return NULL; }
void gui_resize_list(tab_t *tab, int new_size) { (void)tab; (void)new_size; }
uint8_t odroid_settings_SortMode_get(void) { return ODROID_SORT_NAME; }

/* ---- test fixture helpers ----------------------------------------------- */
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

static void reset_fixture(void)
{
    remove(FAVORITES_FILE);
    remove(FAVORITES_TMP);
}

static void write_raw(const char *content)
{
    FILE *f = fopen(FAVORITES_FILE, "w");
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

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

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/** Read the whole favorites file into `out` (NUL-terminated). Returns length,
 * or -1 if the file is missing. For byte-exact comparisons. */
static long read_all(char *out, size_t cap)
{
    FILE *f = fopen(FAVORITES_FILE, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return (long)n;
}

/* ---- tests --------------------------------------------------------------- */

/* Sanity: add/contains still work identically on the SD0 build (append path is
 * shared, only remove/delete differ). */
static void test_sd0_add_contains(void)
{
    reset_fixture();
    CHECK(rg_favorites_add("/roms/nes/mario.nes") == true, "SD0: add first favorite succeeds");
    CHECK(rg_favorites_add("/roms/snes/zelda.sfc") == true, "SD0: add second favorite succeeds");
    CHECK(rg_favorites_contains("/roms/nes/mario.nes") == true, "SD0: first favorite listed");
    CHECK(rg_favorites_contains("/roms/nes/luigi.nes") == false, "SD0: unrelated path not listed");
    CHECK(count_raw_lines() == 2, "SD0: file has exactly the two appended lines");
}

/* The RAM-rewrite remove drops the middle entry and keeps the neighbours. */
static void test_sd0_remove_middle_keeps_others(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    rg_favorites_add("/roms/nes/b.nes");
    rg_favorites_add("/roms/nes/c.nes");
    CHECK(rg_favorites_remove("/roms/nes/b.nes") == true, "SD0: remove of middle entry succeeds");
    CHECK(rg_favorites_contains("/roms/nes/a.nes") == true, "SD0: entry before removed one survives");
    CHECK(rg_favorites_contains("/roms/nes/c.nes") == true, "SD0: entry after removed one survives");
    CHECK(rg_favorites_contains("/roms/nes/b.nes") == false, "SD0: removed entry is gone");
    CHECK(count_raw_lines() == 2, "SD0: file shrinks by exactly one line");
}

/* Removing an absent path rewrites unchanged content and still succeeds. */
static void test_sd0_remove_nonexistent_is_noop(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    rg_favorites_add("/roms/nes/b.nes");
    CHECK(rg_favorites_remove("/roms/nes/not-there.nes") == true,
          "SD0: removing an absent path still reports success");
    CHECK(rg_favorites_contains("/roms/nes/a.nes") == true, "SD0: entry a survives");
    CHECK(rg_favorites_contains("/roms/nes/b.nes") == true, "SD0: entry b survives");
    CHECK(count_raw_lines() == 2, "SD0: no line dropped");
}

/* Removing the sole/last entry leaves a real but empty file (the kept_len==0
 * fwrite-skipped branch), not a deleted file. */
static void test_sd0_remove_last_leaves_empty_file(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/only.nes");
    CHECK(count_raw_lines() == 1, "SD0: single entry present before remove");
    CHECK(rg_favorites_remove("/roms/nes/only.nes") == true, "SD0: remove of the only entry succeeds");
    CHECK(file_exists(FAVORITES_FILE) == true, "SD0: file still exists after emptying (truncated, not deleted)");
    CHECK(file_size(FAVORITES_FILE) == 0, "SD0: emptied favorites file is zero bytes");
    CHECK(count_raw_lines() == 0, "SD0: no lines remain");
    CHECK(rg_favorites_contains("/roms/nes/only.nes") == false, "SD0: emptied file contains nothing");
}

/* Remove on a fresh boot (no file) must fail cleanly, not crash. */
static void test_sd0_remove_when_file_absent(void)
{
    reset_fixture();
    CHECK(file_exists(FAVORITES_FILE) == false, "SD0: fixture starts with no favorites file");
    CHECK(rg_favorites_remove("/roms/nes/mario.nes") == false,
          "SD0: remove with no favorites file reports failure, not a crash");
}

/* Blank lines are dropped by the RAM rewrite even when the targeted path is
 * absent (the line[0]=='\0' filter). */
static void test_sd0_blank_lines_dropped(void)
{
    reset_fixture();
    write_raw("\n/roms/nes/a.nes\n\n/roms/nes/b.nes\n/roms/nes/c.nes");

    CHECK(rg_favorites_contains("/roms/nes/c.nes") == true,
          "SD0: final entry with no trailing newline is found");
    CHECK(rg_favorites_remove("/roms/nes/does-not-exist.nes") == true, "SD0: no-op remove triggers rewrite");
    CHECK(count_raw_lines() == 3, "SD0: blank lines dropped by the rewrite");
    CHECK(raw_has_line("/roms/nes/a.nes") && raw_has_line("/roms/nes/b.nes") && raw_has_line("/roms/nes/c.nes"),
          "SD0: all three real entries preserved across the rewrite");
}

/* Every matching line is removed, not just the first (hand-corrupted dupes). */
static void test_sd0_remove_drops_all_duplicates(void)
{
    reset_fixture();
    write_raw("/roms/nes/a.nes\n/roms/nes/dup.nes\n/roms/nes/dup.nes\n/roms/nes/b.nes\n");
    CHECK(count_raw_lines() == 4, "SD0: fixture has the duplicate line twice");
    CHECK(rg_favorites_remove("/roms/nes/dup.nes") == true, "SD0: remove of a duplicated path succeeds");
    CHECK(rg_favorites_contains("/roms/nes/dup.nes") == false, "SD0: duplicated path fully removed");
    CHECK(count_raw_lines() == 2, "SD0: both duplicate lines dropped, only a/b remain");
}

/* A no-op remove over already-normalized content reproduces it byte-for-byte
 * (one entry per line, exactly one trailing '\n' per kept line). */
static void test_sd0_content_byte_exact(void)
{
    reset_fixture();
    write_raw("/roms/nes/a.nes\n/roms/nes/b.nes\n");

    CHECK(rg_favorites_remove("/roms/nes/absent.nes") == true, "SD0: no-op remove over normalized content succeeds");

    char buf[512];
    long n = read_all(buf, sizeof buf);
    CHECK(n == (long)strlen("/roms/nes/a.nes\n/roms/nes/b.nes\n"),
          "SD0: rewritten file has the exact original byte length");
    CHECK(strcmp(buf, "/roms/nes/a.nes\n/roms/nes/b.nes\n") == 0,
          "SD0: rewritten file is byte-exact (one trailing newline per line)");
}

/* A remove that keeps entries writes exactly one '\n' after each survivor,
 * even if the source's final line lacked a trailing newline. */
static void test_sd0_remove_normalizes_trailing_newline(void)
{
    reset_fixture();
    /* last line has NO trailing newline */
    write_raw("/roms/nes/a.nes\n/roms/nes/b.nes\n/roms/nes/c.nes");
    CHECK(rg_favorites_remove("/roms/nes/b.nes") == true, "SD0: remove middle from newline-less-tail file");

    char buf[512];
    read_all(buf, sizeof buf);
    CHECK(strcmp(buf, "/roms/nes/a.nes\n/roms/nes/c.nes\n") == 0,
          "SD0: survivors each terminated by exactly one newline");
}

/* reset() deletes the file via fs_delete(); a second reset on the now-absent
 * file is tolerated (LFS_ERR_NOENT). */
static void test_sd0_reset(void)
{
    reset_fixture();
    rg_favorites_add("/roms/nes/a.nes");
    CHECK(file_exists(FAVORITES_FILE) == true, "SD0: file exists before reset");
    CHECK(rg_favorites_reset() == true, "SD0: reset reports success");
    CHECK(file_exists(FAVORITES_FILE) == false, "SD0: file removed by reset (fs_delete)");
    CHECK(rg_favorites_reset() == true, "SD0: reset on already-absent file tolerated (LFS_ERR_NOENT)");
}

/* Larger list still round-trips and remove works inside it. */
static void test_sd0_many_entries(void)
{
    reset_fixture();
    char path[64];
    const int n = 200;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "/roms/nes/game%03d.nes", i);
        rg_favorites_add(path);
    }
    CHECK(count_raw_lines() == n, "SD0: all 200 entries appended");
    CHECK(rg_favorites_remove("/roms/nes/game100.nes") == true, "SD0: remove works inside a large file");
    CHECK(rg_favorites_contains("/roms/nes/game100.nes") == false, "SD0: removed entry gone from large file");
    CHECK(count_raw_lines() == n - 1, "SD0: large file shrinks by exactly one line");
}

int main(void)
{
    if (mkdir("/tmp/favtest0", 0755) != 0 && errno != EEXIST) {
        perror("mkdir /tmp/favtest0");
        return 1;
    }

    test_sd0_add_contains();
    test_sd0_remove_middle_keeps_others();
    test_sd0_remove_nonexistent_is_noop();
    test_sd0_remove_last_leaves_empty_file();
    test_sd0_remove_when_file_absent();
    test_sd0_blank_lines_dropped();
    test_sd0_remove_drops_all_duplicates();
    test_sd0_content_byte_exact();
    test_sd0_remove_normalizes_trailing_newline();
    test_sd0_reset();
    test_sd0_many_entries();

    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
