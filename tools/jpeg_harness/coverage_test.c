/* Coverage completeness for hw_jpeg_decoder.c, NOT a regression pin.
 *
 * lock_test.c / callback_test.c / floor_test.c each drive the real file
 * through the one shipped bug they're named after, and are RED against a
 * specific historical pre-fix commit. This file has no such commit to be RED
 * against -- it exists only to reach real, still-live entry points and
 * branches those three don't happen to touch (JPEG_DecodeGetSize, the
 * SrcSize==0 guard, the 4:2:0/4:2:2 chroma paths, the oversized-image
 * rejection, a malformed-header GetInfo failure, JPEG_DecodeDeInit), so gcov
 * measures more of the file that actually ships than three bug-shaped tests
 * happen to reach. tools/jpeg_harness/run.sh runs this GREEN-only, against
 * current hw_jpeg_decoder.c -- see lock_test.c's header for how the fake
 * HAL underneath it is built and cited.
 */
#include <stdio.h>
#include <stdint.h>

#include "main.h"
#include "hw_jpeg_decoder.h"
#include "fakejpeg_control.h"
#include "fakejpeg_buf.h"

/* hw_jpeg_decoder.c's own device-HUD diagnostic globals (non-static, no
 * declaration in hw_jpeg_decoder.h -- see the file's own comment on g_jpeg_*
 * for what each one means). Reading them here checks the actual variables
 * a BSOD/HUD dump would show, not a proxy for them. */
extern uint32_t g_jpeg_hal, g_jpeg_err, g_jpeg_rej, g_jpeg_sub, g_jpeg_need;

static uint8_t g_work_buf[4096];
static uint8_t g_small_work_buf[64];   /* too small for anything but a tiny image */
static uint8_t g_dest[4096];
static uint8_t g_src[512];

static int g_failures = 0;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) printf("  OK   %-58s -> %u\n", what, got);
    else { printf("  FAIL %-58s -> got %u, want %u\n", what, got, want); g_failures++; }
}

int main(void)
{
    printf("=== coverage_test [current hw_jpeg_decoder.c only -- completeness, not a bug pin] ===\n");

    fakejpeg_reset();
    fakejpeg_configure(8, 8, JPEG_444_SUBSAMPLING);
    fakejpeg_fill(g_src, sizeof g_src, 256);

    expect("JPEG_DecodeToBufferInit", JPEG_DecodeToBufferInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);

    printf("\nJPEG_DecodeGetSize -- the real entry point gui.c:1048 uses to size a cover\n"
           "before deciding whether to decode it at all:\n\n");
    uint32_t w = 0, h = 0;
    expect("JPEG_DecodeGetSize decodes", JPEG_DecodeGetSize(fakejpeg_addr(g_src), 256, &w, &h), 0);
    expect("  ...and reports the configured width", w, 8);
    expect("  ...and reports the configured height", h, 8);

    printf("\nJPEG_Run's own guard against a null/empty source (SrcAddress==0 or\n"
           "SrcSize==0) -- every caller can hit this on a failed prior load:\n\n");
    expect("SrcSize==0 is rejected before HAL is ever touched",
           JPEG_DecodeToFrame(fakejpeg_addr(g_src), 0, fakejpeg_addr(g_dest), 0, 0, 0xFF), 1);

    printf("\nchroma subsampling paths COPY_JpegOutInit branches on (4:2:0 and 4:2:2 --\n"
           "the callback/floor/lock tests only ever exercise 4:4:4):\n\n");
    fakejpeg_configure(20, 20, JPEG_420_SUBSAMPLING); /* width%16 != 0 exercises the inputLineOffset calc too */
    expect("4:2:0 image decodes", JPEG_DecodeToFrame(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), 0, 0, 0xFF), 0);
    expect("  ...HAL_JPEG_InfoReadyCallback saw 4:2:0", g_jpeg_sub, JPEG_420_SUBSAMPLING);

    fakejpeg_configure(20, 20, JPEG_422_SUBSAMPLING);
    expect("4:2:2 image decodes", JPEG_DecodeToBuffer(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), &w, &h, 0xFF), 0);
    expect("  ...HAL_JPEG_InfoReadyCallback saw 4:2:2", g_jpeg_sub, JPEG_422_SUBSAMPLING);

    /* 4:4:4 with a width NOT a multiple of 8 -- the other tests' 8x8 images
     * never take this branch (8%8==0), so COPY_JpegOutInit's own inputLineOffset
     * correction goes untested without this. */
    fakejpeg_configure(20, 20, JPEG_444_SUBSAMPLING);
    expect("4:4:4, width%8!=0, image decodes",
           JPEG_DecodeToBuffer(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), &w, &h, 0xFF), 0);

    printf("\nan image too large for its output buffer must be rejected before any of it\n"
           "is written -- HAL_JPEG_InfoReadyCallback's own bounds check, hw_jpeg_decoder.c:249:\n\n");
    expect("JPEG_DecodeToBufferInit (small work buffer)",
           JPEG_DecodeToBufferInit(fakejpeg_addr(g_small_work_buf), sizeof g_small_work_buf), 0);
    fakejpeg_configure(320, 240, JPEG_420_SUBSAMPLING); /* MCU_ROUND(320,16)*MCU_ROUND(240,16)*3/2, way over 64B */
    expect("oversized image rejected", JPEG_DecodeToBuffer(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), &w, &h, 0xFF), 1);
    expect("  ...g_jpeg_rej says \"too large\" (2)", g_jpeg_rej, 2);
    expect("  ...g_jpeg_need reports the size it actually needed", g_jpeg_need, 320u * 240u * 3u / 2u);

    printf("\na malformed header GetInfo can't parse must also be rejected, distinctly\n"
           "from \"too large\" -- g_jpeg_rej==1, not 2:\n\n");
    expect("JPEG_DecodeToFrameInit (back to the normal work buffer)",
           JPEG_DecodeToFrameInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);
    fakejpeg_configure(8, 8, JPEG_444_SUBSAMPLING);
    fakejpeg_force_getinfo_failure();
    expect("unparseable header rejected", JPEG_DecodeToFrame(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), 0, 0, 0xFF), 1);
    expect("  ...g_jpeg_rej says \"GetInfo failed\" (1)", g_jpeg_rej, 1);

    printf("\na good frame still decodes right after a rejection -- neither rejection\n"
           "path above should be able to wedge the decoder for the next caller:\n\n");
    expect("next frame after two rejections decodes fine",
           JPEG_DecodeToFrame(fakejpeg_addr(g_src), 256, fakejpeg_addr(g_dest), 0, 0, 0xFF), 0);

    printf("\nJPEG_DecodeDeInit -- called when a session hands the peripheral back:\n\n");
    expect("JPEG_DecodeDeInit", JPEG_DecodeDeInit(), 0);

    printf("\n%s\n\n", g_failures ? "FAIL" : "PASS: real entry points and branches outside the three named bugs "
                                              "all behave as hw_jpeg_decoder.c documents them");
    return g_failures ? 1 : 0;
}
