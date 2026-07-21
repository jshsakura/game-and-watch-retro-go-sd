/* Why every video frame came back "hal=2 err=0" -- and why the fix is the
 * fix. Drives the REAL hw_jpeg_decoder.c (JPEG_DecodeToBufferInit/ToBuffer
 * for the cover subsystem, JPEG_DecodeToFrameInit/ToFrame for video) against
 * a faithful fake of the ST HAL it's written to (tools/jpeg_harness/hal_fake,
 * transcribed from Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_jpeg.c with
 * line citations). gcov sees every line of the real file this exercises.
 *
 * HAL_JPEG_Decode begins with __HAL_LOCK(hjpeg), which returns HAL_BUSY at
 * once if the handle is locked (stm32h7xx_hal_jpeg.c:1641). The driver clears
 * that lock in exactly two places (:520 and :541), both inside
 * `if (State == HAL_JPEG_STATE_RESET)`. HAL_JPEG_Init restores State and
 * ErrorCode but, called on a handle that is merely READY, never touches Lock.
 *
 * The device runs the cover subsystem and the video player over the SAME
 * static JPEG handle in hw_jpeg_decoder.c, each initializing it once at their
 * own startup (gui.c:271 JPEG_DecodeToBufferInit; video_play.c:416
 * video_decode_init() -> JPEG_DecodeToFrameInit). If a cover-load session
 * ever leaves the handle LOCKED -- production only ever saw the symptom
 * (hal=2 err=0 rej=0 on every frame), never a diagnosed C-level cause, since
 * HAL_JPEG_Decode unlocks on every one of its own return paths -- a later
 * video session's Init call on the OLD code does not know to clear it, and
 * every frame of that session opens locked. video_decode_init() now drives
 * the handle to RESET before Init, so Init clears the lock regardless of
 * what left it that way.
 *
 * fakejpeg_inject_stuck_lock() drives exactly that precondition (see
 * fakejpeg_control.h for why it has to work this way); everything after it
 * is the real JPEG_DecodeToFrameInit/ToFrame call chain, unmodified.
 *
 *   cc ... -o lock_test lock_test.c hw_jpeg_decoder.c hal_jpeg_fake.c   # GREEN
 *   (same, but hw_jpeg_decoder.c from before 7ae5c0e8)                 # RED
 * both done by tools/jpeg_harness/run.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "main.h"              /* JPEG_444_SUBSAMPLING etc. -- fake HAL constants */
#include "hw_jpeg_decoder.h"
#include "fakejpeg_control.h"
#include "fakejpeg_buf.h"

#define LCD_X_SIZE 320
#define LCD_Y_SIZE 240

static uint8_t g_work_buf[4096];   /* JPEG_Buffer given to *Init -- internal YCbCr staging */
static uint8_t g_fb[LCD_X_SIZE * LCD_Y_SIZE * 2];  /* video frame destination */
static uint8_t g_cover_dest[4096]; /* cover destination (JPEG_DecodeToBuffer) */
static uint8_t g_src[256];         /* one small, well-formed 8x8 4:4:4 "image" */

static int g_failures = 0;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("  OK   %-56s -> %u\n", what, got);
    } else {
        printf("  FAIL %-56s -> got %u, want %u\n", what, got, want);
        g_failures++;
    }
}

int main(void)
{
#ifdef PRE_FIX_BUILD
    printf("=== lock_test [pre-fix hw_jpeg_decoder.c -- must FAIL] ===\n");
#else
    printf("=== lock_test [current hw_jpeg_decoder.c -- must PASS] ===\n");
#endif

    fakejpeg_reset();
    fakejpeg_configure(8, 8, JPEG_444_SUBSAMPLING);   /* ImgSize=8*8*3=192B, well under g_work_buf */
    fakejpeg_fill(g_src, sizeof g_src, sizeof g_src); /* EOI is the buffer's last 2 bytes */

    uint32_t w, h;

    printf("\ncover subsystem starts first, decodes one cover successfully:\n\n");
    expect("JPEG_DecodeToBufferInit", JPEG_DecodeToBufferInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);
    expect("cover decode",
           JPEG_DecodeToBuffer(fakejpeg_addr(g_src), sizeof g_src, fakejpeg_addr(g_cover_dest), &w, &h, 0xFF), 0);

    printf("\na fault (never diagnosed on the device beyond its symptom) leaves the\n"
           "handle LOCKED before the video session starts:\n\n");
    fakejpeg_inject_stuck_lock();

    printf("\nvideo_decode_init() -> JPEG_DecodeToFrameInit, then five good frames --\n"
           "this is the exact call sequence video_play.c:416 + video_decode.c:109 make:\n\n");
    expect("JPEG_DecodeToFrameInit", JPEG_DecodeToFrameInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);

    int shown = 0;
    for (int f = 0; f < 5; f++) {
        uint32_t rc = JPEG_DecodeToFrame(fakejpeg_addr(g_src), sizeof g_src, fakejpeg_addr(g_fb), 0, 0, 0xFF);
        printf("      frame %d: rc=%u\n", f, rc);
        if (rc == 0) shown++;
    }
    printf("      -> %d/5 frames decoded\n", shown);

    /* Asserted unconditionally -- NOT weakened under PRE_FIX_BUILD -- so this
     * is the RED signal: against pre-fix hw_jpeg_decoder.c, old JPEG_DecodeInit
     * never reset State, so Init (called on a still-READY handle) skipped the
     * gated unlock and frame 0 opens locked and is rejected. (HAL_JPEG_Abort's
     * own unconditional tail unlock then clears it for frame 1 onward -- a
     * one-frame drop, not every frame forever, but still a real,
     * real-entry-point-observable regression: 4/5, not 5/5.) */
    expect("all 5 frames decoded (JPEG_HandleReset cleared the stale lock)", shown, 5);

    printf("\n%s\n\n", g_failures
           ? "FAIL"
           : "PASS: a handle left locked before a new *Init session is exactly what "
             "JPEG_HandleReset protects against");
    return g_failures ? 1 : 0;
}
