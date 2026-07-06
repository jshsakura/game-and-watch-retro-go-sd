#ifndef STUB_FAV_RG_STORAGE_H
#define STUB_FAV_RG_STORAGE_H
/* rg_favorites.c only needs rg_stat_t + rg_storage_stat (favorites_fill_files
 * checks a favorited ROM still exists on disk before listing it). */
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Real config.h has RG_STORAGE_ROOT "" (this device's SD root), so
 * /roms/<system>/... paths start with exactly this prefix — needed by
 * rg_favorites.c's system_for_path(). */
#ifndef RG_BASE_PATH_ROMS
#define RG_BASE_PATH_ROMS "/roms"
#endif

typedef struct
{
    const char *basename;
    const char *extension;
    size_t size;
    time_t mtime;
    bool is_dir;
    bool is_file;
    bool is_link;
    bool exists;
} rg_stat_t;

rg_stat_t rg_storage_stat(const char *path);

#endif
