#pragma once
/* Minimal FrogFS stub for host tests of rg_storage.c (SD_CARD=0).
 * Provides only the read-only subset rg_storage.c uses:
 *   frogfs_get_entry, frogfs_is_dir, frogfs_is_file, frogfs_opendir,
 *   frogfs_readdir, frogfs_closedir, frogfs_get_name, frogfs_stat.
 * Backed by an in-memory tree the test builds with fake_frogfs_* helpers. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    FROGFS_ENTRY_TYPE_DIR,
    FROGFS_ENTRY_TYPE_FILE,
} frogfs_entry_type_t;

typedef enum {
    FROGFS_COMP_ALGO_NONE,
    FROGFS_COMP_ALGO_ZLIB,
    FROGFS_COMP_ALGO_HEATSHRINK,
    FROGFS_COMP_ALGO_GZIP,
} frogfs_comp_algo_t;

typedef struct frogfs_fs_t frogfs_fs_t;
typedef struct frogfs_entry_t frogfs_entry_t;

typedef struct frogfs_stat_t {
    frogfs_entry_type_t type;
    size_t size;
    frogfs_comp_algo_t compression;
    size_t compressed_sz;
} frogfs_stat_t;

/* Directory handle: rg_storage.c treats this as opaque, so we can add a
 * cursor for iteration state. */
typedef struct frogfs_dh_t {
    const frogfs_fs_t *fs;
    const frogfs_entry_t *entry;
    long cursor;
} frogfs_dh_t;

const frogfs_entry_t *frogfs_get_entry(const frogfs_fs_t *fs, const char *path);
char *frogfs_get_name(const frogfs_entry_t *entry);
int frogfs_is_dir(const frogfs_entry_t *entry);
int frogfs_is_file(const frogfs_entry_t *entry);
void frogfs_stat(const frogfs_fs_t *fs, const frogfs_entry_t *entry, frogfs_stat_t *st);
frogfs_dh_t *frogfs_opendir(frogfs_fs_t *fs, const frogfs_entry_t *entry);
void frogfs_closedir(frogfs_dh_t *dh);
const frogfs_entry_t *frogfs_readdir(frogfs_dh_t *dh);

/* --- test-facing helpers (implemented in fake_frogfs.c) --- */
void fake_frogfs_reset(void);
/* mounted != 0 -> rg_frogfs_get() returns a valid fs; 0 -> returns NULL. */
void fake_frogfs_mount(int mounted);
void fake_frogfs_add(const char *path, int is_dir, size_t size);
