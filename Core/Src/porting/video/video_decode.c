// One-frame JPEG decode into the framebuffer — see video_decode.h.
//
// Uses the STM32 HARDWARE JPEG codec (the same peripheral the launcher uses for
// cover art) instead of software tjpgd: the JPEG core decodes to YCbCr and DMA2D
// converts straight to RGB565 in the LCD framebuffer. That drops per-frame decode
// from ~27ms (tjpgd) to a few ms, which is what makes smooth full-rate playback
// possible. Self-contained: only this file calls the HW decoder, no shared code
// is touched.
//
// The frame is read into RAM, its size is read cheaply from the JPEG header
// (centered as a letterbox), then decoded directly to the framebuffer. The HW
// codec cannot downscale, so a frame larger than the screen is skipped (the
// player keeps the previous frame) — the encoder always targets 320x240 anyway.

#include "video_decode.h"
#include "gw_lcd.h"
#include "main.h"               // wdog_refresh + CMSIS cache ops
#include "hw_jpeg_decoder.h"
#include "music_cover.h"        // g_scratch
#include <string.h>

// g_scratch (Music's 352KB cover scratch, idle while a video plays) is split:
//   [0 .. FRAME_MAX)            the current frame's JPEG bytes (decoder source)
//   [FRAME_MAX .. +JWORK_SZ)    the JPEG core's YCbCr intermediate (MDMA target)
#define FRAME_MAX  (64 * 1024)
#define JWORK_SZ   (160 * 1024)        // >= 320x240 4:2:0 (115KB) with margin
extern uint8_t g_scratch[];

// Diagnostics for the last decode attempt (shown on screen when a clip won't play):
// st 0=ok 1=bad-args/too-big 2=fread-fail 3=jpeg_dims-fail 4=larger-than-screen
// 5=HW-decode-rc-nonzero. w/h = parsed dims, rc = JPEG_DecodeToFrame return.
int  g_vdec_st = 0, g_vdec_w = 0, g_vdec_h = 0;
long g_vdec_sz = 0, g_vdec_rc = 0;
unsigned char g_vdec_b0 = 0, g_vdec_b1 = 0;   // first 2 bytes of the frame (FFD8 = JPEG SOI)

// Split timing (ms): the SD fread vs the HW JPEG decode, so the HUD can tell
// whether the ~40ms/frame cost is read-bound (slow SD) or decode-bound.
int g_vdec_read_ms = 0, g_vdec_jpeg_ms = 0;

void video_decode_init(void)
{
    JPEG_DecodeToFrameInit((uint32_t)(g_scratch + FRAME_MAX), JWORK_SZ);
}

void video_decode_deinit(void)
{
    JPEG_DecodeDeInit();
}

// Read the image size from the JPEG SOF marker — cheap (a header walk) versus a
// full pre-decode, and we need it before placing the letterbox.
static bool jpeg_dims(const uint8_t *p, long n, int *w, int *h)
{
    long i = 2;                                 // past SOI (FF D8)
    while (i + 9 < n) {
        if (p[i] != 0xFF) { i++; continue; }
        uint8_t m = p[i + 1];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {        // SOF0 / SOF1 / SOF2
            *h = (p[i + 5] << 8) | p[i + 6];
            *w = (p[i + 7] << 8) | p[i + 8];
            return *w > 0 && *h > 0;
        }
        if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;                              // standalone marker, no length
            continue;
        }
        i += 2 + ((p[i + 2] << 8) | p[i + 3]);   // skip this length-prefixed segment
    }
    return false;
}

bool video_decode_frame(FILE *f, long size, uint16_t *fb, int fb_w, int fb_h)
{
    g_vdec_sz = size; g_vdec_st = 0; g_vdec_w = g_vdec_h = 0; g_vdec_rc = 0;
    if (!f || size < 2 || size > FRAME_MAX || !fb) { g_vdec_st = 1; return false; }

    wdog_refresh();
    uint32_t t_read0 = HAL_GetTick();
    if (fread(g_scratch, 1, (size_t)size, f) != (size_t)size) { g_vdec_st = 2; return false; }
    g_vdec_read_ms = (int)(HAL_GetTick() - t_read0);
    wdog_refresh();
    g_vdec_b0 = g_scratch[0]; g_vdec_b1 = g_scratch[1];

    int w, h;
    if (!jpeg_dims(g_scratch, size, &w, &h)) { g_vdec_st = 3; return false; }
    g_vdec_w = w; g_vdec_h = h;
    if (w > fb_w || h > fb_h) { g_vdec_st = 4; return false; }   // HW codec can't downscale
    int x = (fb_w - w) / 2, y = (fb_h - h) / 2;

    if (w < fb_w || h < fb_h)
        memset(fb, 0, (size_t)fb_w * fb_h * sizeof(uint16_t));   // letterbox bars

    // Flush the JPEG source to RAM so the peripheral's MDMA reads fresh bytes.
    SCB_CleanDCache_by_Addr((uint32_t *)g_scratch, (int32_t)((size + 31) & ~31L));

    uint32_t t_jpeg0 = HAL_GetTick();
    g_vdec_rc = (long)JPEG_DecodeToFrame((uint32_t)g_scratch, (uint32_t)fb,
                                         (uint16_t)x, (uint16_t)y, 255);
    g_vdec_jpeg_ms = (int)(HAL_GetTick() - t_jpeg0);
    if (g_vdec_rc != 0) { g_vdec_st = 5; return false; }
    return true;
}
