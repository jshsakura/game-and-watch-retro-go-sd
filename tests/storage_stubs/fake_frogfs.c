/* In-memory FrogFS fake for host tests (SD_CARD=0).
 * A flat table of entries keyed by full path; directory children are the
 * entries whose parent path matches. Enough to exercise rg_storage.c's
 * scandir / adjacent-file logic and the "not mounted" (fs==NULL) path. */
#include "frogfs/frogfs.h"
#include "gw_littlefs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_ENTRIES 64
#define MAX_PATH    256

struct frogfs_entry_t {
    char path[MAX_PATH];
    frogfs_entry_type_t type;
    size_t size;
    int used;
};

struct frogfs_fs_t {
    int dummy;
};

static struct frogfs_entry_t s_entries[MAX_ENTRIES];
static struct frogfs_fs_t s_fs;
static int s_mounted = 0;

/* --- test helpers --- */

void fake_frogfs_reset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_mounted = 0;
    fake_lfs_reset();          /* clear the parallel LittleFS tree */
    fake_frogfs_set_route(NULL); /* default: everything routes to FrogFS */
}

void fake_frogfs_mount(int mounted)
{
    s_mounted = mounted;
}

void fake_frogfs_add(const char *path, int is_dir, size_t size)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used) {
            s_entries[i].used = 1;
            strncpy(s_entries[i].path, path, MAX_PATH - 1);
            s_entries[i].path[MAX_PATH - 1] = '\0';
            s_entries[i].type = is_dir ? FROGFS_ENTRY_TYPE_DIR : FROGFS_ENTRY_TYPE_FILE;
            s_entries[i].size = size;
            return;
        }
    }
}

/* rg_frogfs_get() lives here because rg_frogfs.c is not compiled in tests. */
frogfs_fs_t *rg_frogfs_get(void)
{
    return s_mounted ? &s_fs : NULL;
}

/* --- frogfs read API --- */

const frogfs_entry_t *frogfs_get_entry(const frogfs_fs_t *fs, const char *path)
{
    (void)fs;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].path, path) == 0)
            return &s_entries[i];
    }
    return NULL;
}

char *frogfs_get_name(const frogfs_entry_t *entry)
{
    if (!entry)
        return NULL;
    const char *slash = strrchr(entry->path, '/');
    const char *name = slash ? slash + 1 : entry->path;
    return strdup(name);
}

int frogfs_is_dir(const frogfs_entry_t *entry)
{
    return entry && entry->type == FROGFS_ENTRY_TYPE_DIR;
}

int frogfs_is_file(const frogfs_entry_t *entry)
{
    return entry && entry->type == FROGFS_ENTRY_TYPE_FILE;
}

void frogfs_stat(const frogfs_fs_t *fs, const frogfs_entry_t *entry, frogfs_stat_t *st)
{
    (void)fs;
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
    if (!entry)
        return;
    st->type = entry->type;
    st->size = entry->size;
    st->compression = FROGFS_COMP_ALGO_NONE;
}

frogfs_dh_t *frogfs_opendir(frogfs_fs_t *fs, const frogfs_entry_t *entry)
{
    if (!entry || entry->type != FROGFS_ENTRY_TYPE_DIR)
        return NULL;
    frogfs_dh_t *dh = calloc(1, sizeof(*dh));
    if (!dh)
        return NULL;
    dh->fs = fs;
    dh->entry = entry;
    dh->cursor = 0;
    return dh;
}

void frogfs_closedir(frogfs_dh_t *dh)
{
    free(dh);
}

/* Return whether `child` sits directly inside directory `dir`. */
static int is_direct_child(const char *dir, const char *child)
{
    size_t dlen = strlen(dir);
    int root = (strcmp(dir, "/") == 0);

    if (root) {
        if (child[0] != '/')
            return 0;
        /* exactly one leading slash, no further slash after it */
        return strchr(child + 1, '/') == NULL && child[1] != '\0';
    }

    if (strncmp(child, dir, dlen) != 0 || child[dlen] != '/')
        return 0;
    /* nothing but the basename after "dir/" */
    return strchr(child + dlen + 1, '/') == NULL && child[dlen + 1] != '\0';
}

const frogfs_entry_t *frogfs_readdir(frogfs_dh_t *dh)
{
    if (!dh || !dh->entry)
        return NULL;
    const char *dir = ((const struct frogfs_entry_t *)dh->entry)->path;

    for (long i = dh->cursor; i < MAX_ENTRIES; i++) {
        if (!s_entries[i].used)
            continue;
        if (is_direct_child(dir, s_entries[i].path)) {
            dh->cursor = i + 1;
            return &s_entries[i];
        }
    }
    dh->cursor = MAX_ENTRIES;
    return NULL;
}

/* ===================================================================== */
/* Routing (FrogFS vs LittleFS) + an in-memory LittleFS tree.            */
/* ===================================================================== */
#include "gw_littlefs.h"

/* --- path/backend routing --- */

static bool (*s_route)(const char *path); /* NULL => everything is FrogFS */

void fake_frogfs_set_route(bool (*fn)(const char *path))
{
    s_route = fn;
}

/* Mirrors Core/Src/syscalls.c path_has_prefix_dir(): a leading '/' is optional,
 * and `dir` must be a whole path component (followed by '/' or end-of-string). */
static bool route_prefix_dir(const char *path, const char *dir)
{
    if (!path)
        return false;
    if (path[0] == '/')
        path++;
    size_t len = strlen(dir);
    return strncmp(path, dir, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

bool fake_frogfs_route_syscalls(const char *path)
{
    return route_prefix_dir(path, "roms") ||
           route_prefix_dir(path, "covers") ||
           route_prefix_dir(path, "bios") ||
           route_prefix_dir(path, "fonts") ||
           route_prefix_dir(path, "font");
}

bool gw_fs_is_frogfs_path(const char *path)
{
    return s_route ? s_route(path) : true;
}

/* --- in-memory LittleFS tree (parallel to the FrogFS table above) --- */

#define MAX_LFS_ENTRIES 64
#define MAX_LFS_PATH    512

struct lfs_ent {
    char path[MAX_LFS_PATH];
    int is_dir;
    uint32_t size;
    int used;
};

static struct lfs_ent s_lfs[MAX_LFS_ENTRIES];

void fake_lfs_reset(void)
{
    memset(s_lfs, 0, sizeof(s_lfs));
}

void fake_lfs_add(const char *path, int is_dir, uint32_t size)
{
    for (int i = 0; i < MAX_LFS_ENTRIES; i++) {
        if (!s_lfs[i].used) {
            s_lfs[i].used = 1;
            strncpy(s_lfs[i].path, path, MAX_LFS_PATH - 1);
            s_lfs[i].path[MAX_LFS_PATH - 1] = '\0';
            s_lfs[i].is_dir = is_dir;
            s_lfs[i].size = size;
            return;
        }
    }
}

/* Whether `child` sits directly inside directory `dir` (same rule as the
 * FrogFS is_direct_child above, kept separate to avoid coupling the tables). */
static int lfs_is_direct_child(const char *dir, const char *child)
{
    if (strcmp(dir, "/") == 0) {
        if (child[0] != '/')
            return 0;
        return strchr(child + 1, '/') == NULL && child[1] != '\0';
    }
    size_t dlen = strlen(dir);
    if (strncmp(child, dir, dlen) != 0 || child[dlen] != '/')
        return 0;
    return strchr(child + dlen + 1, '/') == NULL && child[dlen + 1] != '\0';
}

static const struct lfs_ent *lfs_find(const char *path)
{
    for (int i = 0; i < MAX_LFS_ENTRIES; i++)
        if (s_lfs[i].used && strcmp(s_lfs[i].path, path) == 0)
            return &s_lfs[i];
    return NULL;
}

int fs_diropen(lfs_dir_t *d, const char *path)
{
    if (!d || !path)
        return LFS_ERR_NOENT;

    /* "/" is always a directory; otherwise it must exist as a dir entry. */
    if (strcmp(path, "/") != 0) {
        const struct lfs_ent *e = lfs_find(path);
        if (!e || !e->is_dir)
            return LFS_ERR_NOENT;
    }

    strncpy(d->path, path, sizeof(d->path) - 1);
    d->path[sizeof(d->path) - 1] = '\0';
    d->cursor = -2; /* emit "." then ".." then real children, like LittleFS */
    d->open = 1;
    return 0;
}

/* >0 = entry filled, 0 = end of directory, <0 = error (lfs_dir_read semantics). */
int fs_dirread(lfs_dir_t *d, struct lfs_info *info)
{
    if (!d || !info || !d->open)
        return -1;

    if (d->cursor == -2) {
        d->cursor = -1;
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        strcpy(info->name, ".");
        return 1;
    }
    if (d->cursor == -1) {
        d->cursor = 0;
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        strcpy(info->name, "..");
        return 1;
    }

    for (long i = d->cursor; i < MAX_LFS_ENTRIES; i++) {
        if (!s_lfs[i].used)
            continue;
        if (!lfs_is_direct_child(d->path, s_lfs[i].path))
            continue;

        d->cursor = i + 1;
        const char *slash = strrchr(s_lfs[i].path, '/');
        const char *name = slash ? slash + 1 : s_lfs[i].path;
        strncpy(info->name, name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        info->type = s_lfs[i].is_dir ? LFS_TYPE_DIR : LFS_TYPE_REG;
        info->size = s_lfs[i].size;
        return 1;
    }

    d->cursor = MAX_LFS_ENTRIES;
    return 0;
}

int fs_dirclose(lfs_dir_t *d)
{
    if (d)
        d->open = 0;
    return 0;
}
