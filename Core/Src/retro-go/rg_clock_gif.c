/* Animated GIF background for the Clock app — see rg_clock_gif.h.
 *
 * Designed for a full 320x240 GIF: at that size one RGB565 frame is 150 KB, so
 * caching every frame won't fit a sane budget. Instead we keep the GIF open and
 * decode ONE frame at a time, on its delay, into a single render buffer, then
 * scale-fill it into the LCD. That handles any size and frame count; the cost
 * is a per-frame LZW decode, which is exactly why this animation level is
 * labelled "high" battery.
 *
 * MEMORY: a 320x240 GIF needs ~270 KB (indices + RGB565 canvas + LZW
 * table; frames compose straight into the 565 canvas, no extra buffer and
 * no per-pixel conversion at blit) — far beyond the ~80 KB launcher heap,
 * whose malloc ASSERTS on OOM instead of returning NULL (the 0131 boot
 * crash). The decode arena comes from clock_overlay_arena() (rg_clock_ring.c):
 * everything in RAM_EMU past .overlay_clock's own footprint, which is free
 * for the duration the clock app is open. A load with no room simply fails
 * (CLOCK_GIF_NO_RAM) -> background stays solid. */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include "gifdec.h"
#include "gw_lcd.h"
#include "gw_malloc.h"
#include "rg_clock_state.h"   /* clock_overlay_arena — decode arena past .overlay_clock's own footprint */
#include <string.h>
#include <stdio.h>
#include "rg_clock_gif.h"

#ifndef GIF_PATH
#define GIF_PATH "/clock/gif/bg.gif"
#endif

/* Chosen background GIF basename (settings picker). We keep only a POINTER into
 * the caller's persistent buffer (rg_clock's s_bgfile) — not a copy — so the
 * launcher's tight DTCM gains no resident string buffer; the full path is built
 * on the stack per load. NULL/"" = the default /clock/gif/bg.gif, so an old cfg
 * with no bgfile entry keeps working. */
static const char *s_gif_name;   /* basename under /clock/gif, or NULL for bg.gif */

void clock_gif_set_file(const char *name)
{
    s_gif_name = (name && name[0]) ? name : NULL;
}

/* Resolve the active GIF path into `out` (or the static default string). */
static const char *gif_resolve_path(char *out, size_t n)
{
    if (!s_gif_name) return GIF_PATH;
    snprintf(out, n, "/clock/gif/%s", s_gif_name);
    return out;
}

static gd_GIF  *s_gif;
static int      s_gw, s_gh;
static uint32_t s_next_tick;
static bool     s_have_frame;
static size_t   s_ram_mark;   /* emu-RAM bump-pointer snapshot (see header) */
static int      s_status = CLOCK_GIF_OK;
static char     s_diag[64] = "";

/* Decode arena, borrowed from rg_emulators' shared_files buffer at load time
 * (see clock_gif_load). The emu-RAM pool is a bump allocator with no free —
 * once the launcher caches covers there may be ~41K left, while a 320x240 GIF
 * needs 273K ("no RAM need 273K free 41K" in the field) — so the pool can
 * never be relied on for the decode. */
static uint8_t *s_arena;
static size_t   s_arena_size;
static size_t   s_arena_off;

int clock_gif_status(void) { return s_status; }
const char *clock_gif_diag(void) { return s_diag; }

/* gifdec allocators over the reserved arena (LIFO reset per load). */
static void *arena_malloc(size_t n)
{
    n = (n + 3) & ~(size_t)3;
    if (!s_arena || s_arena_off + n > s_arena_size) return NULL;
    void *p = s_arena + s_arena_off;
    s_arena_off += n;
    return p;
}
static void *arena_calloc(size_t c, size_t n)
{
    void *p = arena_malloc(c * n);
    if (p) memset(p, 0, c * n);
    return p;
}

/* gifdec free = no-op both ways: the arena is reset per load, the pool path is
 * rolled back at clock_gif_free() via ram_release(). */
static void gif_arena_free(void *p) { (void)p; }

/* clock_gif_reserve() (the boot-time no-op call site in rg_main.c) now lives in
 * rg_clock_ring.c: it must be resident-callable even before/without the clock
 * app ever staging this overlay. */

bool clock_gif_ready(void) { return s_gif != NULL; }

void clock_gif_free(void)
{
    if (s_gif) { gd_close_gif(s_gif); s_gif = NULL; }   /* frees are no-ops */
    if (s_ram_mark) { ram_release(s_ram_mark); s_ram_mark = 0; }
    s_arena_off = 0;                                    /* arena stays reserved */
    s_gw = s_gh = 0; s_next_tick = 0; s_have_frame = false;
}

bool clock_gif_load(void)
{
    clock_gif_free();

    char pbuf[64];
    const char *path = gif_resolve_path(pbuf, sizeof pbuf);

    /* Probe the header ourselves: separates "no file" / "not a GIF" / "no RAM"
     * (the launcher's covers+list already hold much of the emu-RAM pool, so a
     * full-screen GIF may simply not fit what's left). */
    int probe = open(path, O_RDONLY);
    if (probe < 0) { s_status = CLOCK_GIF_NO_FILE;
        snprintf(s_diag, sizeof s_diag, "no file: %s", path); return false; }
    uint8_t hdr[10];
    int hn = read(probe, hdr, 10);
    close(probe);
    if (hn < 10 || memcmp(hdr, "GIF", 3) != 0) { s_status = CLOCK_GIF_BAD_FMT;
        snprintf(s_diag, sizeof s_diag, "not a GIF (got %02X %02X %02X)", hdr[0],hdr[1],hdr[2]); return false; }
    int gw = hdr[6] | (hdr[7] << 8), gh = hdr[8] | (hdr[9] << 8);
    if (gw <= 0 || gh <= 0 || gw > 640 || gh > 480) { s_status = CLOCK_GIF_BAD_DIMS;
        snprintf(s_diag, sizeof s_diag, "bad dims %dx%d (max 480x320)", gw, gh); return false; }
    /* gifdec needs frame+565 canvas (3*w*h) + LZW table (~40KB) + slack */
    size_t need = (size_t)gw * gh * 3 + 48 * 1024;
    /* Decode arena: everything in RAM_EMU past .overlay_clock's own bss end —
     * see clock_overlay_arena() (rg_clock_ring.c) for why this replaced the old
     * "borrow the launcher's shared_files buffer" trick: that pointer was
     * computed BEFORE the clock itself became an overlay, so it aliased this
     * very code once .overlay_clock started loading at __RAM_EMU_START__ too. */
    size_t arena_bytes = 0;
    uint8_t *arena = clock_overlay_arena(&arena_bytes);
    if (!arena || arena_bytes < need) {
        s_status = CLOCK_GIF_NO_RAM;
        snprintf(s_diag, sizeof s_diag, "no RAM: need %dK have %dK",
                 (int)(need/1024), (int)(arena_bytes/1024));
        return false;
    }
    s_arena = arena; s_arena_size = arena_bytes; s_arena_off = 0;
    gd_set_allocator(arena_malloc, arena_calloc, gif_arena_free);

    gd_GIF *g = gd_open_gif(path);
    if (!g) { s_status = CLOCK_GIF_BAD_FMT;
        snprintf(s_diag, sizeof s_diag, "decoder rejected %dx%d", gw, gh);
        clock_gif_free(); return false; }
    if (g->width <= 0 || g->height <= 0 || g->width > 640 || g->height > 480) {
        s_status = CLOCK_GIF_BAD_DIMS;
        gd_close_gif(g);   /* closes the fd; arena frees are no-ops */
        clock_gif_free(); return false;
    }
    s_status = CLOCK_GIF_OK; s_diag[0] = 0;
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
    gd_render_frame(s_gif, s_gif->canvas);   /* composites into the 565 canvas */
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
    const uint16_t *cv = s_gif->canvas;   /* already RGB565 */
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
        const uint16_t *srow = cv + (size_t)(y * h / GW_LCD_HEIGHT) * w;
        uint16_t *drow = fb + y * GW_LCD_WIDTH;
        for (int x = 0; x < GW_LCD_WIDTH; x++)
            drow[x] = srow[(size_t)(x * w / GW_LCD_WIDTH)];
    }
}
