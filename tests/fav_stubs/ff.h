#ifndef STUB_FAV_FF_H
#define STUB_FAV_FF_H
/* Host stand-in for FatFs: rg_favorites.c only calls f_unlink/f_rename, so
 * map them onto POSIX remove()/rename() (real F_NO_FILE distinguished via
 * errno, matching FatFs semantics closely enough for rg_favorites_reset's
 * "already gone is fine" check). */
#include <stdio.h>
#include <errno.h>

typedef enum {
    FR_OK = 0,
    FR_NO_FILE,
    FR_DISK_ERR,
} FRESULT;

static inline FRESULT f_unlink(const char *path)
{
    if (remove(path) == 0)
        return FR_OK;
    if (errno == ENOENT)
        return FR_NO_FILE;
    return FR_DISK_ERR;
}

static inline FRESULT f_rename(const char *path_old, const char *path_new)
{
    return (rename(path_old, path_new) == 0) ? FR_OK : FR_DISK_ERR;
}

#endif
