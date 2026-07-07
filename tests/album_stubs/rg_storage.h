#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifndef RG_PATH_MAX
#define RG_PATH_MAX 255
#endif

typedef struct {
    char path[RG_PATH_MAX + 1];
    const char *basename;
    const char *dirname;
    size_t size;
    time_t mtime;
    bool is_file;
    bool is_dir;
} rg_scandir_t;

typedef int (rg_scandir_cb_t)(const rg_scandir_t *file, void *arg);

enum {
    RG_SCANDIR_FILES = (1 << 0),
    RG_SCANDIR_CONTINUE = 1,
    RG_SCANDIR_SKIP = 2,
    RG_SCANDIR_STOP = 0,
};

bool rg_storage_scandir(const char *path, rg_scandir_cb_t *cb, void *arg, uint32_t flags);
