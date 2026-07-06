#pragma once
/* host stub: minimal LittleFS surface for rg_favorites.c's SD_CARD=0 build.
 * That branch only needs the delete primitive (fav_delete / rg_favorites_reset)
 * and the "not found" error code — the RAM-rewrite in rg_favorites_remove()
 * uses plain stdio, never the LittleFS API. fs_delete() is implemented in the
 * test as a thin wrapper over POSIX remove(). */
#include <stdbool.h>

/* Matches gw_littlefs's LFS_ERR_NOENT (LittleFS returns this for a missing
 * path; rg_favorites treats it as a tolerated "already gone"). */
#define LFS_ERR_NOENT (-2)

int fs_delete(const char *path);
