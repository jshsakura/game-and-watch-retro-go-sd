// One-frame JPEG decode into the framebuffer — see video_decode.h.
//
// Any frame size is accepted: tjpgd downscales (1/2..1/8) until the frame fits
// 320x240, then it is centered (letterboxed). Frames smaller than the screen
// are centered as-is. A frame that will not decode returns false so the player
// keeps the previous frame instead of flashing garbage.

#include "video_decode.h"
#include "gw_lcd.h"
#include "tjpgd.h"
#include <string.h>

#define VID_WORK_SZ (32 * 1024)

typedef struct { FILE *f; long remain; } vsrc_t;

static uint8_t   s_work[VID_WORK_SZ];
static uint16_t *s_fb;
static int       s_ox, s_oy;          // top-left of the centered image
static int       s_clip_w, s_clip_h;  // framebuffer bounds

// Stream the JPEG payload from the file (bounded to the chunk length).
static size_t vid_in(JDEC *jd, uint8_t *buf, size_t len)
{
    vsrc_t *s = (vsrc_t *)jd->device;
    if ((long)len > s->remain) len = (size_t)s->remain;
    if (buf) {
        size_t got = fread(buf, 1, len, s->f);
        s->remain -= (long)got;
        return got;
    }
    if (fseek(s->f, (long)len, SEEK_CUR) != 0) return 0;   // skip
    s->remain -= (long)len;
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

    JDEC jd;
    vsrc_t src = { f, size };
    if (jd_prepare(&jd, vid_in, s_work, sizeof s_work, &src) != JDR_OK)
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

    return jd_decomp(&jd, vid_out, sc) == JDR_OK;
}
