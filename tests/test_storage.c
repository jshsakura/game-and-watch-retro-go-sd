/* Host unit tests for Core/Src/retro-go/rg_storage.c.
 *
 * The SAME source is compiled twice — once per storage backend — against
 * fake FatFs (SD_CARD=1) and fake FrogFS (SD_CARD=0) layers in
 * tests/storage_stubs/. POSIX-backed operations (mkdir/stat/exists) run
 * against a real mkdtemp() sandbox; directory listing / adjacent-file logic
 * runs against the fake backend for the config under test.
 *
 * Compile + run (also recorded in tests/test_storage.build):
 *   gcc -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=1 -Itests/storage_stubs \
 *       -ICore/Inc/retro-go tests/test_storage.c Core/Src/retro-go/rg_storage.c \
 *       tests/storage_stubs/stubs.c tests/storage_stubs/fake_fatfs.c \
 *       tests/storage_stubs/posix_dir.c -o /tmp/mtest/test_storage_sd1
 *   /tmp/mtest/test_storage_sd1
 *
 *   gcc -O2 -Wall -Wextra -std=gnu11 -DSD_CARD=0 -Itests/storage_stubs \
 *       -ICore/Inc/retro-go tests/test_storage.c Core/Src/retro-go/rg_storage.c \
 *       tests/storage_stubs/stubs.c tests/storage_stubs/fake_frogfs.c \
 *       -o /tmp/mtest/test_storage_sd0
 *   /tmp/mtest/test_storage_sd0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rg_storage.h"
#if SD_CARD == 0
#include "frogfs/frogfs.h"
#include "gw_littlefs.h"
#endif

/* Provided by tests/storage_stubs/stubs.c (mirrors rg_utils.c). */
const char *rg_basename(const char *path);

/* ---- tiny test harness (matches tests/test_clock_alarm.c conventions) ---- */
static int g_failures = 0;

#define CHECK(cond, msg)                              \
    do {                                              \
        if (!(cond)) {                                \
            printf("  FAIL: %s\n", (msg));            \
            g_failures++;                             \
        }                                             \
    } while (0)

#define OK(name) printf("OK %s\n", (name))

#if SD_CARD == 1
#define CFG_LABEL "SD_CARD=1 (FatFs)"
#else
#define CFG_LABEL "SD_CARD=0 (FrogFS)"
#endif

/* ------------------------------------------------------------------------- */

static char g_tmp[256];

static void make_sandbox(void)
{
    char tmpl[] = "/tmp/rgstor_XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) {
        perror("mkdtemp");
        exit(2);
    }
    strncpy(g_tmp, d, sizeof(g_tmp) - 1);
    g_tmp[sizeof(g_tmp) - 1] = '\0';
}

static void write_file(const char *path, size_t nbytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    for (size_t i = 0; i < nbytes; i++)
        fputc('x', f);
    fclose(f);
}

/* scandir counting callback */
typedef struct {
    int count;
    int stop_after; /* 0 = never stop */
    int last_is_dir;
    int last_is_file;
} count_ctx_t;

static int count_cb(const rg_scandir_t *file, void *arg)
{
    count_ctx_t *c = arg;
    c->count++;
    c->last_is_dir = file->is_dir;
    c->last_is_file = file->is_file;
    if (c->stop_after && c->count >= c->stop_after)
        return RG_SCANDIR_STOP;
    return RG_SCANDIR_CONTINUE;
}

/* ---- storage readiness / mount gating ---------------------------------- */

static void test_ready(void)
{
#if SD_CARD == 0
    /* FrogFS: ready == disk_mounted && rg_frogfs_get() != NULL. */
    fake_frogfs_reset();     /* mounted = 0 */
    rg_storage_init();       /* disk_mounted = true */
    CHECK(!rg_storage_ready(), "not ready when frogfs is unmounted (NULL)");
    fake_frogfs_mount(1);
    CHECK(rg_storage_ready(), "ready once frogfs is mounted");
#else
    /* FatFs: ready == disk_mounted. */
    rg_storage_init();
    CHECK(rg_storage_ready(), "ready after init");
#endif
    OK("ready gating (" CFG_LABEL ")");
}

/* ---- CHECK_PATH empty/NULL-path guard ---------------------------------- */

static void test_checkpath_guard(void)
{
    char prev[RG_PATH_MAX + 1];
    char next[RG_PATH_MAX + 1];
    count_ctx_t ctx = {0};

    CHECK(!rg_storage_mkdir(""), "mkdir rejects empty path");
    CHECK(!rg_storage_exists(""), "exists rejects empty path");
    CHECK(!rg_storage_delete(""), "delete rejects empty path");
    CHECK(!rg_storage_scandir("", count_cb, &ctx, 0), "scandir rejects empty path");
    CHECK(!rg_storage_scandir(NULL, count_cb, &ctx, 0), "scandir rejects NULL path");
    CHECK(!rg_storage_get_adjacent_files("", prev, next), "adjacent rejects empty path");
    OK("CHECK_PATH empty/NULL guard (" CFG_LABEL ")");
}

/* ---- mkdir (POSIX-backed, both configs) -------------------------------- */

static void test_mkdir(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/a/b/c", g_tmp);

    CHECK(rg_storage_mkdir(path), "mkdir creates nested dirs (missing parents)");

    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode), "created path is a directory");

    /* idempotent: second call succeeds via EEXIST branch */
    CHECK(rg_storage_mkdir(path), "mkdir on existing dir returns true (EEXIST)");
    OK("mkdir nested + idempotent (" CFG_LABEL ")");
}

/* ---- exists + stat (POSIX-backed, both configs) ------------------------ */

static void test_exists_stat(void)
{
    char file[512];
    snprintf(file, sizeof(file), "%s/hello.txt", g_tmp);
    write_file(file, 42);

    CHECK(rg_storage_exists(file), "exists true for real file");
    char missing[512];
    snprintf(missing, sizeof(missing), "%s/nope.txt", g_tmp);
    CHECK(!rg_storage_exists(missing), "exists false for missing file");

    rg_stat_t s = rg_storage_stat(file);
    CHECK(s.exists, "stat: exists true");
    CHECK(s.is_file, "stat: is_file true");
    CHECK(!s.is_dir, "stat: is_dir false");
    CHECK(s.size == 42, "stat: size matches");
    CHECK(s.basename && strcmp(s.basename, "hello.txt") == 0, "stat: basename");
    CHECK(s.extension && strcmp(s.extension, "txt") == 0, "stat: extension");

    rg_stat_t sd = rg_storage_stat(g_tmp);
    CHECK(sd.exists && sd.is_dir && !sd.is_file, "stat: directory flags");

    rg_stat_t sm = rg_storage_stat(missing);
    CHECK(!sm.exists, "stat: missing file -> exists false");

    rg_stat_t sn = rg_storage_stat(NULL);
    CHECK(!sn.exists, "stat: NULL path -> exists false");
    OK("exists + stat (" CFG_LABEL ")");
}

/* ---- backend-specific fixture: a directory "list" with files + a subdir - */

#if SD_CARD == 1
static const char *make_list_dir(void)
{
    static char dir[512];
    char p[600];
    snprintf(dir, sizeof(dir), "%s/list", g_tmp);
    mkdir(dir, 0777);
    snprintf(p, sizeof(p), "%s/a.txt", dir); write_file(p, 1);
    snprintf(p, sizeof(p), "%s/b.txt", dir); write_file(p, 2);
    snprintf(p, sizeof(p), "%s/c.txt", dir); write_file(p, 3);
    snprintf(p, sizeof(p), "%s/z.dat", dir); write_file(p, 4); /* diff ext */
    snprintf(p, sizeof(p), "%s/sub", dir);   mkdir(p, 0777);   /* a subdir */
    return dir;
}
#else
static const char *make_list_dir(void)
{
    fake_frogfs_reset();
    fake_frogfs_mount(1);
    fake_frogfs_add("/list", 1, 0);
    fake_frogfs_add("/list/a.txt", 0, 1);
    fake_frogfs_add("/list/b.txt", 0, 2);
    fake_frogfs_add("/list/c.txt", 0, 3);
    fake_frogfs_add("/list/z.dat", 0, 4);
    fake_frogfs_add("/list/sub", 1, 0);
    return "/list";
}
#endif

/* ---- scandir: filtering, STOP, missing dir ----------------------------- */

static void test_scandir(void)
{
    const char *dir = make_list_dir();

    count_ctx_t all = {0};
    CHECK(rg_storage_scandir(dir, count_cb, &all, 0), "scandir returns true");
    CHECK(all.count == 5, "scandir (flags=0) lists all entries (4 files + 1 dir)");

    count_ctx_t files = {0};
    rg_storage_scandir(dir, count_cb, &files, RG_SCANDIR_FILES);
    CHECK(files.count == 4, "scandir FILES filter -> 4 files");

    count_ctx_t dirs = {0};
    rg_storage_scandir(dir, count_cb, &dirs, RG_SCANDIR_DIRS);
    CHECK(dirs.count == 1, "scandir DIRS filter -> 1 dir");

    count_ctx_t stop = { .stop_after = 1 };
    rg_storage_scandir(dir, count_cb, &stop, 0);
    CHECK(stop.count == 1, "scandir honours RG_SCANDIR_STOP after first entry");

    /* non-existent directory -> false */
    count_ctx_t none = {0};
#if SD_CARD == 1
    char missing[512];
    snprintf(missing, sizeof(missing), "%s/does_not_exist", g_tmp);
    CHECK(!rg_storage_scandir(missing, count_cb, &none, 0), "scandir on missing dir -> false");
#else
    CHECK(!rg_storage_scandir("/does_not_exist", count_cb, &none, 0), "scandir on missing dir -> false");
#endif
    OK("scandir filtering + STOP + missing (" CFG_LABEL ")");
}

/* ---- scandir RG_PATH_MAX overflow guard (SD_CARD=1 only) ---------------- */

#if SD_CARD == 1
static void test_scandir_pathlen_guard(void)
{
    /* rg_storage.c: `if (path_len > RG_PATH_MAX - 5) return false;` */
    char longpath[RG_PATH_MAX + 32];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';

    count_ctx_t ctx = {0};
    CHECK(!rg_storage_scandir(longpath, count_cb, &ctx, 0),
          "scandir rejects path longer than RG_PATH_MAX-5 before opendir");
    OK("scandir path-length overflow guard (" CFG_LABEL ")");
}
#endif

/* ---- adjacent-file neighbour scan (both configs) ----------------------- */

static void adjacent_of(const char *dir, const char *basename, char *prev, char *next)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, basename);
    prev[0] = next[0] = '\0';
    int ok = rg_storage_get_adjacent_files(path, prev, next);
    CHECK(ok, "adjacent returns true");
}

static void test_adjacent(void)
{
    const char *dir = make_list_dir();
    char prev[RG_PATH_MAX + 1];
    char next[RG_PATH_MAX + 1];

    /* middle: b.txt -> prev a.txt, next c.txt */
    adjacent_of(dir, "b.txt", prev, next);
    CHECK(strcmp(rg_basename(prev), "a.txt") == 0, "adjacent(b): prev = a.txt");
    CHECK(strcmp(rg_basename(next), "c.txt") == 0, "adjacent(b): next = c.txt");

    /* first: a.txt -> no lower neighbour, prev falls back to self */
    adjacent_of(dir, "a.txt", prev, next);
    CHECK(strcmp(rg_basename(prev), "a.txt") == 0, "adjacent(a): prev wraps to self");
    CHECK(strcmp(rg_basename(next), "b.txt") == 0, "adjacent(a): next = b.txt");

    /* last: c.txt -> no higher neighbour, next falls back to self */
    adjacent_of(dir, "c.txt", prev, next);
    CHECK(strcmp(rg_basename(prev), "b.txt") == 0, "adjacent(c): prev = b.txt");
    CHECK(strcmp(rg_basename(next), "c.txt") == 0, "adjacent(c): next wraps to self");

    /* extension filter: z.dat is the only .dat -> both wrap to self */
    adjacent_of(dir, "z.dat", prev, next);
    CHECK(strcmp(rg_basename(prev), "z.dat") == 0, "adjacent(z.dat): prev wraps (ext-isolated)");
    CHECK(strcmp(rg_basename(next), "z.dat") == 0, "adjacent(z.dat): next wraps (ext-isolated)");

    /* one output pointer NULL is allowed (need_prev only) */
    char only_prev[RG_PATH_MAX + 1] = {0};
    char pth[600];
    snprintf(pth, sizeof(pth), "%s/b.txt", dir);
    CHECK(rg_storage_get_adjacent_files(pth, only_prev, NULL),
          "adjacent with next=NULL still succeeds");
    CHECK(strcmp(rg_basename(only_prev), "a.txt") == 0, "adjacent(next=NULL): prev = a.txt");

    OK("adjacent neighbour scan + wraparound + ext filter (" CFG_LABEL ")");
}

/* ---- FrogFS-not-mounted paths (SD_CARD=0 only) ------------------------- */

#if SD_CARD == 0
static void test_frogfs_unmounted(void)
{
    fake_frogfs_reset();   /* rg_frogfs_get() -> NULL */
    fake_frogfs_mount(0);

    count_ctx_t ctx = {0};
    CHECK(!rg_storage_scandir("/list", count_cb, &ctx, 0),
          "scandir returns false when frogfs unmounted");

    char prev[RG_PATH_MAX + 1] = {0};
    char next[RG_PATH_MAX + 1] = {0};
    CHECK(!rg_storage_get_adjacent_files("/list/b.txt", prev, next),
          "adjacent returns false when frogfs unmounted");

    CHECK(!rg_storage_ready(), "ready false when frogfs unmounted");
    OK("FrogFS-not-mounted paths (" CFG_LABEL ")");
}

/* ---- LittleFS-walk branch (SD_CARD=0, non-FrogFS RW paths) -------------- */
/* When gw_fs_is_frogfs_path() is false, rg_storage_scandir() walks the dir
 * with fs_diropen/fs_dirread/fs_dirclose instead of FrogFS. We install the
 * exact syscalls.c routing rule so e.g. /music is LittleFS but /roms is not,
 * then back the LittleFS API with an in-memory tree (fake_lfs_*). */

/* Callback that records per-entry flags/size/basename for the first N hits. */
typedef struct {
    int count;
    int stop_after;
    char names[16][64];
    int is_dir[16];
    int is_file[16];
    size_t sizes[16];
} rec_ctx_t;

static int rec_cb(const rg_scandir_t *file, void *arg)
{
    rec_ctx_t *c = arg;
    if (c->count < 16) {
        snprintf(c->names[c->count], sizeof(c->names[0]), "%s",
                 file->basename ? file->basename : "");
        c->is_dir[c->count] = file->is_dir;
        c->is_file[c->count] = file->is_file;
        c->sizes[c->count] = file->size;
    }
    c->count++;
    if (c->stop_after && c->count >= c->stop_after)
        return RG_SCANDIR_STOP;
    return RG_SCANDIR_CONTINUE;
}

/* Find the recorded entry named `name`, or -1. */
static int rec_index_of(const rec_ctx_t *c, const char *name)
{
    int n = c->count < 16 ? c->count : 16;
    for (int i = 0; i < n; i++)
        if (strcmp(c->names[i], name) == 0)
            return i;
    return -1;
}

static void littlefs_fixture(void)
{
    fake_frogfs_reset();
    fake_frogfs_mount(1); /* frogfs mounted, but /music routes to LittleFS */
    fake_frogfs_set_route(fake_frogfs_route_syscalls);

    /* A RW LittleFS directory: two files + one subdir (with a nested file). */
    fake_lfs_add("/music", 1, 0);
    fake_lfs_add("/music/song.mp3", 0, 4096);
    fake_lfs_add("/music/tune.mp3", 0, 128);
    fake_lfs_add("/music/album", 1, 0);
    fake_lfs_add("/music/album/deep.mp3", 0, 7);
}

static void test_littlefs_scandir_basic(void)
{
    littlefs_fixture();

    rec_ctx_t all = {0};
    CHECK(rg_storage_scandir("/music", rec_cb, &all, 0),
          "scandir on a LittleFS dir returns true");
    CHECK(all.count == 3, "LittleFS scandir lists 2 files + 1 dir (no '.'/'..' )");

    int fi = rec_index_of(&all, "song.mp3");
    CHECK(fi >= 0, "LittleFS: song.mp3 enumerated");
    if (fi >= 0) {
        CHECK(all.is_file[fi] && !all.is_dir[fi], "LittleFS: song.mp3 is_file, not is_dir");
        CHECK(all.sizes[fi] == 4096, "LittleFS: song.mp3 size reported from lfs_info");
    }
    int di = rec_index_of(&all, "album");
    CHECK(di >= 0, "LittleFS: album subdir enumerated");
    if (di >= 0)
        CHECK(all.is_dir[di] && !all.is_file[di], "LittleFS: album is_dir, not is_file");

    /* basename must point at the last path component, not the full path. */
    CHECK(rec_index_of(&all, "/music/song.mp3") < 0,
          "LittleFS: basename is the leaf, not the full path");

    OK("LittleFS scandir enumerates files+dirs (" CFG_LABEL ")");
}

static void test_littlefs_dotdirs_skipped(void)
{
    littlefs_fixture();
    rec_ctx_t all = {0};
    rg_storage_scandir("/music", rec_cb, &all, 0);
    CHECK(rec_index_of(&all, ".") < 0, "LittleFS: '.' is skipped");
    CHECK(rec_index_of(&all, "..") < 0, "LittleFS: '..' is skipped");
    OK("LittleFS skips '.' and '..' (" CFG_LABEL ")");
}

static void test_littlefs_filters(void)
{
    littlefs_fixture();

    rec_ctx_t files = {0};
    rg_storage_scandir("/music", rec_cb, &files, RG_SCANDIR_FILES);
    CHECK(files.count == 2, "LittleFS FILES filter -> 2 files");

    rec_ctx_t dirs = {0};
    rg_storage_scandir("/music", rec_cb, &dirs, RG_SCANDIR_DIRS);
    CHECK(dirs.count == 1, "LittleFS DIRS filter -> 1 dir");
    OK("LittleFS FILES/DIRS filters (" CFG_LABEL ")");
}

static void test_littlefs_stop(void)
{
    littlefs_fixture();
    rec_ctx_t stop = { .stop_after = 1 };
    rg_storage_scandir("/music", rec_cb, &stop, 0);
    CHECK(stop.count == 1, "LittleFS scandir honours RG_SCANDIR_STOP after first entry");
    OK("LittleFS RG_SCANDIR_STOP (" CFG_LABEL ")");
}

static void test_littlefs_missing_dir(void)
{
    fake_frogfs_reset();
    fake_frogfs_mount(1);
    fake_frogfs_set_route(fake_frogfs_route_syscalls);
    /* /music is a LittleFS path but nothing was added -> fs_diropen fails. */
    rec_ctx_t none = {0};
    CHECK(!rg_storage_scandir("/music", rec_cb, &none, 0),
          "LittleFS scandir on a missing dir returns false");
    CHECK(none.count == 0, "LittleFS missing dir: callback never fired");
    OK("LittleFS missing dir -> false (" CFG_LABEL ")");
}

static void test_littlefs_recursive(void)
{
    littlefs_fixture();

    rec_ctx_t flat = {0};
    rg_storage_scandir("/music", rec_cb, &flat, RG_SCANDIR_FILES);
    CHECK(flat.count == 2, "LittleFS non-recursive FILES -> 2 (top level only)");

    rec_ctx_t deep = {0};
    rg_storage_scandir("/music", rec_cb, &deep, RG_SCANDIR_FILES | RG_SCANDIR_RECURSIVE);
    CHECK(deep.count == 3, "LittleFS recursive FILES -> 3 (includes album/deep.mp3)");
    CHECK(rec_index_of(&deep, "deep.mp3") >= 0, "LittleFS recursion descended into subdir");
    OK("LittleFS RG_SCANDIR_RECURSIVE walks subdirs (" CFG_LABEL ")");
}

static void test_littlefs_pathlen_guard(void)
{
    fake_frogfs_reset();
    fake_frogfs_mount(1);
    fake_frogfs_set_route(fake_frogfs_route_syscalls);

    fake_lfs_add("/music", 1, 0);
    fake_lfs_add("/music/ok.mp3", 0, 1);

    /* An entry whose "/music/<name>" exceeds RG_PATH_MAX+1 must be skipped
     * (snprintf truncation branch), not abort the whole scan. */
    char longname[300];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    char longpath[320];
    snprintf(longpath, sizeof(longpath), "/music/%s", longname);
    fake_lfs_add(longpath, 0, 1);

    rec_ctx_t ctx = {0};
    CHECK(rg_storage_scandir("/music", rec_cb, &ctx, 0),
          "LittleFS scandir still returns true despite an over-long entry");
    CHECK(rec_index_of(&ctx, "ok.mp3") >= 0, "LittleFS: short entry still enumerated");
    CHECK(ctx.count == 1, "LittleFS: over-long entry skipped, not counted or aborted");
    OK("LittleFS path-too-long entry skipped (" CFG_LABEL ")");
}

static void test_littlefs_root_dir(void)
{
    fake_frogfs_reset();
    fake_frogfs_mount(1);
    fake_frogfs_set_route(fake_frogfs_route_syscalls);
    fake_lfs_add("/save", 1, 0); /* "/save" is a LittleFS path under root */

    rec_ctx_t ctx = {0};
    CHECK(rg_storage_scandir("/", rec_cb, &ctx, 0),
          "LittleFS scandir on '/' returns true (root is always a dir)");
    CHECK(rec_index_of(&ctx, "save") >= 0, "LittleFS root lists top-level '/save' entry");
    OK("LittleFS root '/' enumeration (" CFG_LABEL ")");
}

static void test_frogfs_prefixed_path_uses_frogfs(void)
{
    /* With the syscalls router installed, a /roms path is FrogFS: prove the
     * dispatch reaches the FrogFS walk (populated only in the FrogFS tree). */
    fake_frogfs_reset();
    fake_frogfs_mount(1);
    fake_frogfs_set_route(fake_frogfs_route_syscalls);

    fake_frogfs_add("/roms", 1, 0);
    fake_frogfs_add("/roms/nes", 1, 0);
    fake_frogfs_add("/roms/nes/mario.nes", 0, 64);
    /* A decoy LittleFS entry at the same logical spot must NOT be seen. */
    fake_lfs_add("/roms/nes", 1, 0);
    fake_lfs_add("/roms/nes/decoy.bin", 0, 1);

    rec_ctx_t ctx = {0};
    CHECK(rg_storage_scandir("/roms/nes", rec_cb, &ctx, 0),
          "scandir on a FrogFS-prefixed path returns true");
    CHECK(rec_index_of(&ctx, "mario.nes") >= 0, "FrogFS-prefixed path walked the FrogFS tree");
    CHECK(rec_index_of(&ctx, "decoy.bin") < 0, "FrogFS-prefixed path did NOT walk the LittleFS tree");
    CHECK(ctx.count == 1, "FrogFS-prefixed path enumerated exactly the FrogFS entry");
    OK("FrogFS-prefixed path still takes the FrogFS walk (" CFG_LABEL ")");
}
#endif

int main(void)
{
    printf("== rg_storage host tests [%s] ==\n", CFG_LABEL);
    make_sandbox();

    test_ready();
    test_checkpath_guard();
    test_mkdir();
    test_exists_stat();
    test_scandir();
#if SD_CARD == 1
    test_scandir_pathlen_guard();
#endif
    test_adjacent();
#if SD_CARD == 0
    test_frogfs_unmounted();
    test_littlefs_scandir_basic();
    test_littlefs_dotdirs_skipped();
    test_littlefs_filters();
    test_littlefs_stop();
    test_littlefs_missing_dir();
    test_littlefs_recursive();
    test_littlefs_pathlen_guard();
    test_littlefs_root_dir();
    test_frogfs_prefixed_path_uses_frogfs();
#endif

    if (g_failures) {
        printf("FAILED: %d check(s) failed [%s]\n", g_failures, CFG_LABEL);
        return 1;
    }
    printf("ALL PASS [%s]\n", CFG_LABEL);
    return 0;
}
