// On-device JPEG-frame decode benchmark. Sizes the video player's resolution/fps
// budget by measuring how fast the M7 actually decodes one frame: it decodes a
// bundled 320x240 baseline-JPEG frame N times into the real framebuffer (decode +
// blit — the same path the player will use), then shows ms/frame and fps on the
// LCD and waits for a key. Used to answer "does 320x240 @ 24fps fit the 42 ms
// budget?" before the player is built.
#include <odroid_system.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "main.h"            // HAL_GetTick / HAL_Delay
#include "gw_lcd.h"          // lcd_get_active_buffer / lcd_clear_buffers / lcd_swap / GW_LCD_*
#include "rg_i18n.h"         // i18n_draw_text_line
#include "tjpgd.h"
#include "video_bench.h"
#include "video_frame_320.h"   // video_frame_320[] / video_frame_320_len

#define BENCH_FRAMES   30
#define BENCH_WORK_SZ  (32 * 1024)

typedef struct { const uint8_t *p; size_t len, pos; } memsrc_t;

static uint8_t s_work[BENCH_WORK_SZ];

// tjpgd input: stream the in-flash JPEG (or skip bytes when buf == NULL).
static size_t bench_in(JDEC *jd, uint8_t *buf, size_t len)
{
    memsrc_t *s = (memsrc_t *)jd->device;
    size_t n = s->len - s->pos;
    if (n > len) n = len;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

// tjpgd output: RGB565 straight to the active framebuffer (same path the player
// will use, so the cost includes the blit).
static int bench_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    uint16_t *fb = lcd_get_active_buffer();
    const uint16_t *src = (const uint16_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++)
        for (int x = rect->left; x <= rect->right; x++) {
            if (x < GW_LCD_WIDTH && y < GW_LCD_HEIGHT)
                fb[y * GW_LCD_WIDTH + x] = *src;
            src++;
        }
    return 1;
}

static bool decode_one(void)
{
    JDEC jd;
    memsrc_t s = { video_frame_320, video_frame_320_len, 0 };
    if (jd_prepare(&jd, bench_in, s_work, sizeof s_work, &s) != JDR_OK) return false;
    return jd_decomp(&jd, bench_out, 0) == JDR_OK;
}

void video_decode_bench(void)
{
    wdog_refresh();
    bool ok = decode_one();                 // prime + verify it decodes at all

    uint32_t t0 = HAL_GetTick();
    // Refresh the watchdog every frame — the decode loop blocks for ~1s, which
    // would otherwise trip the watchdog and kick the app back to the launcher.
    for (int i = 0; i < BENCH_FRAMES; i++) { wdog_refresh(); decode_one(); }
    uint32_t dt = HAL_GetTick() - t0;       // ms for BENCH_FRAMES frames

    int ms_x10 = (int)((dt * 10) / BENCH_FRAMES);   // tenths of a ms / frame
    int fps    = dt ? (int)((1000u * BENCH_FRAMES) / dt) : 0;

    lcd_clear_buffers();
    char line[48];
    i18n_draw_text_line(20, 60, 280, "video decode  320x240", 0xFFFF, 0, 1);
    if (ok) snprintf(line, sizeof line, "decode: %d.%d ms / frame", ms_x10 / 10, ms_x10 % 10);
    else    snprintf(line, sizeof line, "DECODE FAILED");
    i18n_draw_text_line(20, 90, 280, line, 0xFFFF, 0, 0);
    snprintf(line, sizeof line, "= %d fps (decode + blit only)", fps);
    i18n_draw_text_line(20, 110, 280, line, 0xFFFF, 0, 0);
    i18n_draw_text_line(20, 150, 280, "budget @24fps = 42 ms", 0x7BEF, 0, 0);
    i18n_draw_text_line(20, 175, 280, "press any key to continue", 0x7BEF, 0, 0);
    lcd_swap();

    // Wait for a press, then a release, so it doesn't fall straight through.
    odroid_gamepad_state_t j;
    for (;;) {
        wdog_refresh();   // result screen can sit here a while — keep the dog fed
        odroid_input_read_gamepad(&j);
        int any = 0;
        for (int b = 0; b < ODROID_INPUT_MAX; b++) if (j.values[b]) any = 1;
        if (any) break;
        HAL_Delay(20);
    }
    HAL_Delay(200);
    lcd_clear_buffers();
}
