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
#include "rg_clock_state.h"   /* clock_overlay_arena */
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
/* The path list lives INSIDE the borrowed arena (right after the photo buffer),
 * NOT in resident BSS — 64 * (RG_PATH_MAX+1) would be ~16KB of firmware image. */
static char    (*s_paths)[RG_PATH_MAX + 1] = NULL;

static int scan_cb(const rg_scandir_t *f, void *arg)
{
    (void)arg;
    if (!s_paths || !f->is_file || f->basename[0] == '.') return RG_SCANDIR_CONTINUE;
    const char *ext = rg_extension(f->basename);
    /* .565 = raw RGB565 dump (web tool); .bmp = uncompressed bitmap the user
     * can export from any image editor — both need only a straight read/convert,
     * no decoder (JPEG/PNG live in the music overlay, out of reach here). */
    if (ext && (strcasecmp(ext, "565") == 0 || strcasecmp(ext, "bmp") == 0)
            && s_count < MAX_PHOTOS) {
        snprintf(s_paths[s_count], sizeof s_paths[s_count], "%s", f->path);
        s_count++;
    }
    return RG_SCANDIR_CONTINUE;
}

/* Read exactly `len` bytes or fail (short read = truncated/wrong file). */
static bool read_full(int fd, void *dst, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = read(fd, (uint8_t *)dst + total, len - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total == len;
}

/* Raw RGB565 dump: exactly 320x240x2 bytes, straight into the arena. */
static bool load_565_fd(int fd)
{
    return read_full(fd, s_buf, PHOTO_BYTES);
}

static inline uint16_t bgr888_to_565(uint8_t b, uint8_t g, uint8_t r)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Uncompressed BMP (24/32-bit, 320x240): parse the header, then convert one
 * row at a time BGR->RGB565 into the arena. BMP rows are 4-byte aligned and,
 * for a positive height, stored bottom-up — so row s maps to screen row
 * (239 - s). No decoder, one 320*4-byte row buffer on the stack. */
static bool load_bmp_fd(int fd)
{
    uint8_t hdr[54];
    if (!read_full(fd, hdr, sizeof hdr)) return false;
    if (hdr[0] != 'B' || hdr[1] != 'M') return false;

    uint32_t data_off = (uint32_t)hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
    int32_t  w        = (int32_t)((uint32_t)hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24));
    int32_t  h        = (int32_t)((uint32_t)hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24));
    uint16_t bpp      = (uint16_t)(hdr[28] | (hdr[29] << 8));
    uint32_t comp     = (uint32_t)hdr[30] | (hdr[31] << 8) | (hdr[32] << 16) | (hdr[33] << 24);

    bool top_down = h < 0;
    int32_t hh = top_down ? -h : h;
    if (w != GW_LCD_WIDTH || hh != GW_LCD_HEIGHT) return false;   /* must be 320x240 */
    if (comp != 0 || (bpp != 24 && bpp != 32)) return false;      /* uncompressed BGR(A) only */

    int bypp = bpp / 8;
    size_t stride = (((size_t)w * bypp + 3) & ~(size_t)3);        /* 4-byte aligned rows */
    if (stride > (size_t)GW_LCD_WIDTH * 4) return false;

    /* seek to pixel data (data_off is from file start; we've read 54 bytes) */
    if (data_off < sizeof hdr) return false;
    for (uint32_t skip = data_off - sizeof hdr; skip > 0; ) {
        uint8_t tmp[64];
        size_t take = skip > sizeof tmp ? sizeof tmp : skip;
        if (!read_full(fd, tmp, take)) return false;
        skip -= (uint32_t)take;
    }

    uint8_t row[GW_LCD_WIDTH * 4];
    for (int32_t s = 0; s < GW_LCD_HEIGHT; s++) {
        if (!read_full(fd, row, stride)) return false;
        int dst_y = top_down ? s : (GW_LCD_HEIGHT - 1 - s);
        uint16_t *out = s_buf + (size_t)dst_y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++) {
            const uint8_t *p = row + (size_t)x * bypp;   /* B, G, R (, A) */
            out[x] = bgr888_to_565(p[0], p[1], p[2]);
        }
    }
    return true;
}

/* Load photo #idx into the arena, dispatching on its extension. */
static bool load_photo(int idx)
{
    if (idx < 0 || idx >= s_count || !s_buf || !s_paths) return false;
    int fd = open(s_paths[idx], O_RDONLY);
    if (fd < 0) return false;
    const char *ext = rg_extension(s_paths[idx]);
    bool ok = (ext && strcasecmp(ext, "bmp") == 0) ? load_bmp_fd(fd) : load_565_fd(fd);
    close(fd);
    return ok;   /* wrong-size/format -> reject (guard the blit) */
}

bool clock_album_open(void)
{
    clock_album_close();

    void *arena = (void *)clock_overlay_arena(&s_arena_bytes);
    if (!arena) return false;
    /* Arena layout: [photo 150K][path list ~16K]. Both carved from the space
     * past .overlay_clock's own footprint (clock_overlay_arena); nothing goes
     * into resident BSS. */
    size_t need = PHOTO_BYTES + (size_t)MAX_PHOTOS * (RG_PATH_MAX + 1);
    if (s_arena_bytes < need) return false;   /* safety: never overrun */
    s_buf   = (uint16_t *)arena;
    s_paths = (char (*)[RG_PATH_MAX + 1])((uint8_t *)arena + PHOTO_BYTES);

    s_count = 0; s_index = 0;
    rg_storage_scandir(ALBUM_DIR, scan_cb, NULL, 0);
    if (s_count == 0) { s_buf = NULL; return false; }

    s_ready = load_photo(0);
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
    s_ready = load_photo(s_index);
}

void clock_album_close(void)
{
    /* The arena is the launcher's shared_files, borrowed in place — nothing to
     * free. The clock exit path rebuilds the launcher's lists. */
    s_ready = false; s_buf = NULL; s_paths = NULL; s_arena_bytes = 0; s_count = 0; s_index = 0;
}
