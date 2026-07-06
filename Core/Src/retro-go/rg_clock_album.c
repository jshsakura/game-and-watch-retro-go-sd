/* Photo-album background — see rg_clock_album.h.
 *
 * Borrows the launcher's shared_files buffer as the photo arena. This is safe
 * ONLY because rg_clock rebuilds the launcher's ROM lists on exit (see
 * rg_emulators_reset_all_lists + a tab refresh). We never touch the allocator. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

#include "gw_lcd.h"
#include "rg_emulators.h"   /* rg_emulators_shared_file_buffer */
#include "rg_storage.h"     /* rg_storage_scandir, rg_scandir_t, RG_SCANDIR_* */
#include "rg_utils.h"       /* rg_extension */
#include "rg_clock_album.h"

#define ALBUM_DIR   "/clock/album"
#define PHOTO_PX    (GW_LCD_WIDTH * GW_LCD_HEIGHT)   /* 320 * 240 */
#define PHOTO_BYTES ((size_t)PHOTO_PX * 2)           /* raw RGB565 file size */
#define MAX_PHOTOS  64

static uint16_t *s_buf   = NULL;   /* current photo: PHOTO_BYTES inside the borrowed arena */
static size_t    s_arena_bytes = 0;
static int       s_count = 0;
static int       s_index = 0;
static bool      s_ready = false;
static char      s_paths[MAX_PHOTOS][RG_PATH_MAX + 1];

static int scan_cb(const rg_scandir_t *f, void *arg)
{
    (void)arg;
    if (!f->is_file || f->basename[0] == '.') return RG_SCANDIR_CONTINUE;
    const char *ext = rg_extension(f->basename);
    if (ext && strcasecmp(ext, "565") == 0 && s_count < MAX_PHOTOS) {
        snprintf(s_paths[s_count], sizeof s_paths[s_count], "%s", f->path);
        s_count++;
    }
    return RG_SCANDIR_CONTINUE;
}

/* Read photo #idx (raw RGB565, must be exactly PHOTO_BYTES) into the arena. */
static bool load_565(int idx)
{
    if (idx < 0 || idx >= s_count || !s_buf) return false;
    int fd = open(s_paths[idx], O_RDONLY);
    if (fd < 0) return false;
    size_t total = 0;
    while (total < PHOTO_BYTES) {
        int n = read(fd, (uint8_t *)s_buf + total, PHOTO_BYTES - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    return total == PHOTO_BYTES;   /* wrong-size file -> reject (guard the blit) */
}

bool clock_album_open(void)
{
    clock_album_close();

    int maxcount = 0;
    void *arena = (void *)rg_emulators_shared_file_buffer(&maxcount);
    if (!arena || maxcount <= 0) return false;
    /* Byte span of shared_files = slots * slot size (rg_emulators owns the type). */
    s_arena_bytes = rg_emulators_shared_file_bytes();
    if (s_arena_bytes < PHOTO_BYTES) return false;   /* safety: never overrun */
    s_buf = (uint16_t *)arena;

    s_count = 0; s_index = 0;
    rg_storage_scandir(ALBUM_DIR, scan_cb, NULL, 0);
    if (s_count == 0) { s_buf = NULL; return false; }

    s_ready = load_565(0);
    if (!s_ready) s_buf = NULL;
    return s_ready;
}

bool clock_album_ready(void) { return s_ready && s_buf != NULL; }

const uint16_t *clock_album_current(void) { return clock_album_ready() ? s_buf : NULL; }

int clock_album_count(void) { return s_count; }

void clock_album_advance(void)
{
    if (s_count <= 1) return;
    s_index = (s_index + 1) % s_count;
    s_ready = load_565(s_index);
}

void clock_album_close(void)
{
    /* The arena is the launcher's shared_files, borrowed in place — nothing to
     * free. The clock exit path rebuilds the launcher's lists. */
    s_ready = false; s_buf = NULL; s_arena_bytes = 0; s_count = 0; s_index = 0;
}
