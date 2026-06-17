// One-frame JPEG decode into the framebuffer — see video_decode.h.
//
// The whole JPEG frame is read from the file in ONE bulk fread into RAM, then
// decoded from memory. Streaming it through tjpgd's input callback instead meant
// dozens of small reads per frame, and with FF_FS_TINY (a single shared sector
// buffer) each of those is expensive — that per-read overhead, not throughput,
// was what made on-device playback stutter. Decoding from RAM restores the
// bench-measured decode speed.
//
// Any frame size is accepted: tjpgd downscales (1/2..1/8) until the frame fits
// 320x240, then it is centered (letterboxed). A frame that will not decode (or
// is too large to buffer) returns false so the player keeps the previous frame
// instead of flashing garbage.

#include "video_decode.h"
#include "gw_lcd.h"
#include "main.h"           // wdog_refresh
#include "tjpgd.h"
#include <string.h>

#define VID_WORK_SZ   (32 * 1024)            // tjpgd work pool (front of g_scratch)
#define VID_FRAME_OFF VID_WORK_SZ            // frame bytes live after the work pool
#define VID_FRAME_MAX ((352 * 1024) - VID_WORK_SZ)   // rest of g_scratch (~320KB)

// Reuse the Music app's 352KB cover scratch: [0,32KB) tjpgd work area, then the
// rest holds the frame's JPEG bytes. Music and Video share the overlay and never
// run at once, so there is no conflict.
extern uint8_t g_scratch[];

// In-memory JPEG source for tjpgd (replaces the old per-chunk file streamer).
typedef struct { const uint8_t *p; long len; long pos; } vmem_t;

static uint16_t *s_fb;
static int       s_ox, s_oy;          // top-left of the centered image
static int       s_clip_w, s_clip_h;  // framebuffer bounds

static size_t vid_in(JDEC *jd, uint8_t *buf, size_t len)
{
    vmem_t *s = (vmem_t *)jd->device;
    long avail = s->len - s->pos;
    if (avail < 0) avail = 0;
    if ((long)len > avail) len = (size_t)avail;
    if (buf) memcpy(buf, s->p + s->pos, len);     // copy out (decode)
    s->pos += (long)len;                          // or just skip
    return len;
}

// tjpgd RGB565 output -> framebuffer, offset by (s_ox,s_oy), clipped.
static int vid_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        int dy = s_oy + y;
        for (int x = rect->left; x <= rect->right; x++) {
            int dx = s_ox + x;
            uint16_t px = *src++;
            if (dx >= 0 && dx < s_clip_w && dy >= 0 && dy < s_clip_h)
                s_fb[dy * s_clip_w + dx] = px;
        }
    }
    return 1;
}

bool video_decode_frame(FILE *f, long size, uint16_t *fb, int fb_w, int fb_h)
{
    if (!f || size < 2 || !fb) return false;
    if (size > VID_FRAME_MAX)  return false;          // too big to buffer -> skip

    // Pull the entire frame into RAM in one read (the SD-overhead win).
    uint8_t *fbuf = g_scratch + VID_FRAME_OFF;
    wdog_refresh();
    if (fread(fbuf, 1, (size_t)size, f) != (size_t)size) return false;
    wdog_refresh();

    JDEC jd;
    vmem_t src = { fbuf, size, 0 };
    if (jd_prepare(&jd, vid_in, g_scratch, VID_WORK_SZ, &src) != JDR_OK)
        return false;                       // not a baseline JPEG / unreadable

    // Pick the largest tjpgd scale (1, 1/2, 1/4, 1/8) that fits the screen.
    uint8_t sc = 0;
    while (sc < 3 && (((int)jd.width >> sc) > fb_w || ((int)jd.height >> sc) > fb_h))
        sc++;
    int w = (int)jd.width >> sc, h = (int)jd.height >> sc;

    s_fb = fb;
    s_clip_w = fb_w; s_clip_h = fb_h;
    s_ox = (fb_w - w) / 2;
    s_oy = (fb_h - h) / 2;

    memset(fb, 0, (size_t)fb_w * fb_h * sizeof(uint16_t));   // clean letterbox each frame
    return jd_decomp(&jd, vid_out, sc) == JDR_OK;
}
