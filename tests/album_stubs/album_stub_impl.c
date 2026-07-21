/* Test-side backing for the album host test: a real malloc'd arena and a
 * scandir that replays a settable file list. Compiled into the test binary. */
#include <stdlib.h>
#include <string.h>
#include "rg_clock_state.h"
#include "rg_storage.h"
#include <stdio.h>

#define GW_LCD_WIDTH  320
#define GW_LCD_HEIGHT 240
#define ARENA_BYTES   ((size_t)GW_LCD_WIDTH * GW_LCD_HEIGHT * 2 + 64 * (RG_PATH_MAX + 1) + 4096)

static unsigned char g_arena[ARENA_BYTES];

uint8_t *clock_overlay_arena(size_t *out_bytes)
{
    if (out_bytes) *out_bytes = ARENA_BYTES;
    return g_arena;
}

/* settable file list for the scandir replay */
static const char **g_files;
static int g_nfiles;
void album_test_set_files(const char **paths, int n) { g_files = paths; g_nfiles = n; }

bool rg_storage_scandir(const char *path, rg_scandir_cb_t *cb, void *arg, uint32_t flags)
{
    (void)path; (void)flags;
    for (int i = 0; i < g_nfiles; i++) {
        rg_scandir_t f; memset(&f, 0, sizeof f);
        snprintf(f.path, sizeof f.path, "%s", g_files[i]);
        const char *slash = strrchr(g_files[i], '/');
        f.basename = slash ? slash + 1 : g_files[i];
        f.is_file = true;
        if (cb(&f, arg) == RG_SCANDIR_STOP) break;
    }
    return true;
}
