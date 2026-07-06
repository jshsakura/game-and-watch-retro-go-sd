/* Animated GIF background for the Clock app — see rg_clock_gif.h.
 *
 * Designed for a full 320x240 GIF: at that size one RGB565 frame is 150 KB, so
 * caching every frame won't fit a sane budget. Instead we keep the GIF open and
 * decode ONE frame at a time, on its delay, into a single render buffer, then
 * scale-fill it into the LCD. That handles any size and frame count; the cost
 * is a per-frame LZW decode, which is exactly why this animation level is
 * labelled "high" battery.
 *
 * MEMORY: a 320x240 GIF needs ~330 KB (gifdec canvas+frame, LZW table;
 * frames compose straight into the canvas) — far beyond the ~80 KB
 * launcher heap, whose malloc ASSERTS on OOM instead of returning NULL (the
 * 0131 boot crash). So everything comes from the big emu-RAM bump pool
 * (ram_malloc, the same pool the launcher uses for covers): we snapshot the
 * bump pointer with ram_mark() at load and roll it back with ram_release()
 * at free, so cover/list allocations keep working afterwards. No emulator is
 * running while the clock shows, and every emulator launch resets the pool
 * anyway. ram_malloc returns NULL on exhaustion -> background stays solid. */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include "gifdec.h"
#include "gw_lcd.h"
#include "gw_malloc.h"
#include <string.h>
#include "rg_clock_gif.h"

#define GIF_PATH "/clock/bg.gif"

static gd_GIF  *s_gif;
static int      s_gw, s_gh;
static uint32_t s_next_tick;
static bool     s_have_frame;
static size_t   s_ram_mark;   /* emu-RAM bump-pointer snapshot (see header) */
static int      s_status = CLOCK_GIF_OK;

int clock_gif_status(void) { return s_status; }

/* gifdec allocator = the emu-RAM arena; free is a no-op, the whole arena is
 * rolled back at clock_gif_free() via ram_release(). */
static void gif_arena_free(void *p) { (void)p; }

bool clock_gif_ready(void) { return s_gif != NULL; }

void clock_gif_free(void)
{
    if (s_gif) { gd_close_gif(s_gif); s_gif = NULL; }   /* frees are no-ops */
    if (s_ram_mark) { ram_release(s_ram_mark); s_ram_mark = 0; }
    s_gw = s_gh = 0; s_next_tick = 0; s_have_frame = false;
}

bool clock_gif_load(void)
{
    clock_gif_free();

    /* Probe the header ourselves: separates "no file" / "not a GIF" / "no RAM"
     * (the launcher's covers+list already hold much of the emu-RAM pool, so a
     * full-screen GIF may simply not fit what's left). */
    int probe = open(GIF_PATH, O_RDONLY);
    if (probe < 0) { s_status = CLOCK_GIF_NO_FILE; return false; }
    uint8_t hdr[10];
    int hn = read(probe, hdr, 10);
    close(probe);
    if (hn < 10 || memcmp(hdr, "GIF", 3) != 0) { s_status = CLOCK_GIF_BAD_FMT; return false; }
    int gw = hdr[6] | (hdr[7] << 8), gh = hdr[8] | (hdr[9] << 8);
    if (gw <= 0 || gh <= 0 || gw > 480 || gh > 320) { s_status = CLOCK_GIF_BAD_DIMS; return false; }
    /* gifdec needs frame(4*w*h) + LZW table (~40KB) + slack */
    size_t need = (size_t)gw * gh * 4 + 48 * 1024;
    if (ram_get_free_size() < need) { s_status = CLOCK_GIF_NO_RAM; return false; }

    s_ram_mark = ram_mark();
    gd_set_allocator(ram_malloc, ram_calloc, gif_arena_free);

    gd_GIF *g = gd_open_gif(GIF_PATH);
    if (!g) { s_status = CLOCK_GIF_BAD_FMT; clock_gif_free(); return false; }
    if (g->width <= 0 || g->height <= 0 || g->width > 480 || g->height > 320) {
        s_status = CLOCK_GIF_BAD_DIMS;
        gd_close_gif(g);   /* closes the fd; arena frees are no-ops */
        clock_gif_free(); return false;
    }
    s_status = CLOCK_GIF_OK;
    s_gif = g; s_gw = g->width; s_gh = g->height;
    s_next_tick = 0; s_have_frame = false;
    return true;
}

/* Decode the next frame (looping at end) into s_rgb; schedule the one after. */
static void gif_decode_next(uint32_t now)
{
    int r = gd_get_frame(s_gif);
    if (r <= 0) {                         /* end of animation -> loop */
        gd_rewind(s_gif);
        r = gd_get_frame(s_gif);
        if (r <= 0) { s_next_tick = now + 1000; return; }   /* empty/broken: back off */
    }
    /* Compose the frame straight into gifdec's own canvas and blit from it —
     * saves a whole extra w*h*3 buffer. Composition-wise this matches what
     * dispose() would do for methods 0/1/2; only the rare "restore previous"
     * (3) loses its pristine canvas, acceptable for a background loop. */
    gd_render_frame(s_gif, s_gif->canvas);
    s_have_frame = true;
    uint16_t delay_cs = s_gif->gce.delay ? s_gif->gce.delay : 10;   /* default 100 ms */
    s_next_tick = now + delay_cs * 10u;
}

void clock_gif_blit(uint16_t *fb, uint32_t now)
{
    if (!s_gif || !fb) return;
    if (!s_have_frame || now >= s_next_tick) gif_decode_next(now);
    if (!s_have_frame) return;

    int w = s_gw, h = s_gh;
    const uint8_t *rgb = s_gif->canvas;
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
        const uint8_t *srow = rgb + (size_t)(y * h / GW_LCD_HEIGHT) * w * 3;
        uint16_t *drow = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++) {
            const uint8_t *p = srow + (size_t)(x * w / GW_LCD_WIDTH) * 3;
            drow[x] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
}
