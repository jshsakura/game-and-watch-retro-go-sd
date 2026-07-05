/* Animated GIF background for the Clock app — see rg_clock_gif.h.
 *
 * Designed for a full 320x240 GIF: at that size one RGB565 frame is 150 KB, so
 * caching every frame won't fit a sane budget. Instead we keep the GIF open and
 * decode ONE frame at a time, on its delay, into a single render buffer, then
 * scale-fill it into the LCD. That handles any size and frame count; the cost
 * is a per-frame LZW decode, which is exactly why this animation level is
 * labelled "high" battery.
 *
 * All allocation is transient — the GIF decoder state and the one RGB frame
 * buffer live in the launcher heap only while the clock GIF is showing (no
 * emulator is running then) and are freed on exit, so an emulator's RAM is
 * never reduced. A failed open/malloc just leaves the background solid. */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "gifdec.h"
#include "gw_lcd.h"
#include "rg_clock_gif.h"

#define GIF_PATH "/clock/bg.gif"

static gd_GIF  *s_gif;
static uint8_t *s_rgb;        /* one decoded frame, w*h*3 (RGB888) */
static int      s_gw, s_gh;
static uint32_t s_next_tick;
static bool     s_have_frame;

bool clock_gif_ready(void) { return s_gif != NULL; }

void clock_gif_free(void)
{
    if (s_gif) { gd_close_gif(s_gif); s_gif = NULL; }
    if (s_rgb) { free(s_rgb); s_rgb = NULL; }
    s_gw = s_gh = 0; s_next_tick = 0; s_have_frame = false;
}

bool clock_gif_load(void)
{
    clock_gif_free();

    gd_GIF *g = gd_open_gif(GIF_PATH);
    if (!g) return false;
    if (g->width <= 0 || g->height <= 0 || g->width > 480 || g->height > 320) {
        gd_close_gif(g); return false;
    }
    uint8_t *rgb = (uint8_t *)malloc((size_t)g->width * g->height * 3);
    if (!rgb) { gd_close_gif(g); return false; }

    s_gif = g; s_rgb = rgb; s_gw = g->width; s_gh = g->height;
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
    gd_render_frame(s_gif, s_rgb);
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
    const uint8_t *rgb = s_rgb;
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
        const uint8_t *srow = rgb + (size_t)(y * h / GW_LCD_HEIGHT) * w * 3;
        uint16_t *drow = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++) {
            const uint8_t *p = srow + (size_t)(x * w / GW_LCD_WIDTH) * 3;
            drow[x] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
}
