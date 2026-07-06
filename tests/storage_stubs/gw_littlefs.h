#pragma once
/* host stub: minimal LittleFS dir API surface for rg_storage.c's SD_CARD=0
 * LittleFS-enumeration branch.
 *
 * By default fake_frogfs.c routes every path to FrogFS via
 * gw_fs_is_frogfs_path() (so the pre-existing SD0 tests, which use /list-style
 * paths, keep hitting the FrogFS walk unchanged). A test that wants to exercise
 * the LittleFS branch installs the syscalls.c-mirroring router with
 * fake_frogfs_set_route(fake_frogfs_route_syscalls) and populates an in-memory
 * LittleFS tree with fake_lfs_add(). fs_diropen/fs_dirread/fs_dirclose then walk
 * that tree (emitting "." and ".." first, like real lfs_dir_read). */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { LFS_TYPE_REG = 0x001, LFS_TYPE_DIR = 0x002 };

/* Matches the LittleFS error codes rg_favorites.c / rg_storage.c care about. */
#define LFS_ERR_NOENT (-2)

struct lfs_info {
    uint8_t type;
    uint32_t size;
    char name[256];
};

/* rg_storage.c treats lfs_dir_t as opaque (declares one on the stack and only
 * passes its address), so the fake can carry iteration state inline. */
typedef struct {
    char path[512];
    long cursor; /* -2 => ".", -1 => "..", >=0 => index into the fake tree */
    int open;
} lfs_dir_t;

int fs_diropen(lfs_dir_t *d, const char *path);
int fs_dirread(lfs_dir_t *d, struct lfs_info *info);
int fs_dirclose(lfs_dir_t *d);

/* --- test-facing helpers for the in-memory LittleFS tree (fake_frogfs.c) --- */
void fake_lfs_reset(void);
void fake_lfs_add(const char *path, int is_dir, uint32_t size);

/* Routing control (see gw_fs_is_frogfs_path in fake_frogfs.c):
 *   default            -> everything is FrogFS (legacy test behaviour)
 *   set_route(fn)       -> install a custom rule (NULL restores the default)
 *   route_syscalls      -> the exact rule from Core/Src/syscalls.c
 *                          (roms/covers/bios/fonts/font are FrogFS, rest LFS) */
void fake_frogfs_set_route(bool (*fn)(const char *path));
bool fake_frogfs_route_syscalls(const char *path);
