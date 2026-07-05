/* Animated GIF background for the Clock app — see rg_clock_gif.h.
 *
 * A GIF is not video: palette-based, few frames, long inter-frame delays. So
 * the cheap way to show one is to pay the decode cost ONCE (on load) and cache
 * every frame as RGB565; playback then only blits the current cached frame at
 * the GIF's delay. No per-frame LZW decode, no SD read during playback. The
 * cache is capped and transient (launcher heap while no emulator runs), so an
 * emulator's RAM is never reduced. */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "gifdec.h"
#include "gw_lcd.h"
#include "rg_clock_gif.h"

#define GIF_PATH        "/clock/bg.gif"
#define GIF_MAX_FRAMES  48
#define GIF_CACHE_BUDGET (384 * 1024)   /* RGB565 cache ceiling (bytes) */

static uint16_t *s_frame[GIF_MAX_FRAMES];
static uint16_t  s_delay_cs[GIF_MAX_FRAMES];    /* per-frame delay, centiseconds */
static int       s_nframes;
static int       s_gw, s_gh;
static int       s_cur;
static uint32_t  s_next_tick;

bool clock_gif_ready(void) { return s_nframes > 0; }

void clock_gif_free(void)
{
    for (int i = 0; i < s_nframes; i++) { free(s_frame[i]); s_frame[i] = NULL; }
    s_nframes = 0; s_gw = s_gh = 0; s_cur = 0; s_next_tick = 0;
}

bool clock_gif_load(void)
{
    clock_gif_free();

    gd_GIF *gif = gd_open_gif(GIF_PATH);
    if (!gif) return false;

    int w = gif->width, h = gif->height;
    if (w <= 0 || h <= 0 || w > 480 || h > 320) { gd_close_gif(gif); return false; }

    size_t fbytes = (size_t)w * h * 2;          /* RGB565 per cached frame */
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { gd_close_gif(gif); return false; }

    size_t used = 0; int n = 0;
    while (n < GIF_MAX_FRAMES && gd_get_frame(gif) > 0) {
        if (used + fbytes > GIF_CACHE_BUDGET) break;   /* cap: keep RAM bounded */
        uint16_t *fr = (uint16_t *)malloc(fbytes);
        if (!fr) break;
        gd_render_frame(gif, rgb);
        for (int i = 0; i < w * h; i++) {
            uint8_t r = rgb[i*3], g = rgb[i*3+1], b = rgb[i*3+2];
            fr[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
        s_frame[n]    = fr;
        s_delay_cs[n] = gif->gce.delay ? gif->gce.delay : 10;   /* default 100 ms */
        used += fbytes; n++;
    }

    free(rgb);
    gd_close_gif(gif);
    if (n == 0) return false;

    s_nframes = n; s_gw = w; s_gh = h; s_cur = 0; s_next_tick = 0;
    return true;
}

void clock_gif_blit(uint16_t *fb, uint32_t now)
{
    if (s_nframes <= 0 || !fb) return;

    /* advance frames by the GIF's own delay — never faster */
    if (s_next_tick == 0) s_next_tick = now + s_delay_cs[s_cur] * 10u;
    else if (now >= s_next_tick) {
        s_cur = (s_cur + 1) % s_nframes;
        s_next_tick = now + s_delay_cs[s_cur] * 10u;
    }

    const uint16_t *src = s_frame[s_cur];
    int w = s_gw, h = s_gh;
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
        const uint16_t *srow = src + (y * h / GW_LCD_HEIGHT) * w;
        uint16_t *drow = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++)
            drow[x] = srow[x * w / GW_LCD_WIDTH];
    }
}
