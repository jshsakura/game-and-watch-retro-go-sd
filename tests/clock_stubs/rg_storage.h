#pragma once
/* host stub: the clock uses rg_storage_mkdir ("/clock", "/clock/album") and
 * rg_storage_scandir (the settings file pickers), neither of which is exercised
 * by the pure-logic tests — mkdir always succeeds and scandir finds nothing (an
 * empty /clock), which is all the linker/logic tests need. */
#include <stdbool.h>
#include <stdint.h>

static inline bool rg_storage_mkdir(const char *dir) { (void)dir; return true; }

#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif
typedef struct {
    char path[RG_PATH_MAX + 1];
    const char *basename;
    bool is_file;
} rg_scandir_t;
typedef int (rg_scandir_cb_t)(const rg_scandir_t *file, void *arg);
enum { RG_SCANDIR_STOP = 0, RG_SCANDIR_CONTINUE = 1, RG_SCANDIR_SKIP = 2 };
static inline bool rg_storage_scandir(const char *path, rg_scandir_cb_t *cb, void *arg, uint32_t flags)
{ (void)path; (void)cb; (void)arg; (void)flags; return true; }
