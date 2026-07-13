/* Why "decode st=5 rc=1" failed every video frame while covers and .gw
 * artwork kept rendering -- and why testing only covers proved nothing about
 * video. Drives the REAL JPEG_DecodeToFrame/JPEG_DecodeToBuffer against the
 * faithful HAL fake in tools/jpeg_harness/hal_fake (see lock_test.c's header
 * for how that fake is built and cited).
 *
 * HAL calls HAL_JPEG_GetDataCallback the instant it has consumed every byte
 * it was given, JpegInCount == InDataLength (stm32h7xx_hal_jpeg.c:3584-3591,
 * JPEG_ReadInputData). For an image handed over WHOLE, exhausting the buffer
 * like that is the normal end of the stream, not an error -- the peripheral
 * still holds the image's tail (including the EOI it was already given) in
 * its input FIFO and finishes from there. Whether this fires at all depends
 * only on whether HAL ever exhausts what it was handed:
 *
 *   caller              SrcSize it passes            vs true JPEG length
 *   covers (gui.c:890)  the cache slot                larger  -> never fires
 *   .gw artwork         rest of the ROM file           larger  -> never fires
 *   video (this bug)    the exact AVI chunk            EQUAL   -> fires every frame
 *
 * The commit that broke this made the callback set decode_rejected = 1 --
 * read "HAL wants more input" as "the image is bad". That is true for a
 * genuinely truncated image, but a truncated image is caught a different
 * way: it never reaches EOC at all, so HAL_JPEG_Decode times out (see
 * floor_test.c) -- st is already non-OK there, decode_rejected doesn't need
 * to say anything.
 *
 * fakejpeg_get_data_callback_count() (hal_fake, not hw_jpeg_decoder.c --
 * see fakejpeg_control.h) is the direct observable of whether HAL asked for
 * more; it is instrumented in the fake's OWN call site for that callback,
 * not in the shipped file.
 */
#include <stdio.h>
#include <stdint.h>

#include "main.h"              /* JPEG_444_SUBSAMPLING etc. -- fake HAL constants */
#include "hw_jpeg_decoder.h"
#include "fakejpeg_control.h"
#include "fakejpeg_buf.h"

static uint8_t g_work_buf[4096];
static uint8_t g_dest[4096];
static uint8_t g_padded_src[65536]; /* true image is its first 256 bytes; rest is padding */
static uint8_t g_trunc_src[64];     /* no EOI anywhere in it -- genuine truncation */

static int g_failures = 0;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) printf("  OK   %-58s -> %u\n", what, got);
    else { printf("  FAIL %-58s -> got %u, want %u\n", what, got, want); g_failures++; }
}

int main(void)
{
#ifdef PRE_FIX_BUILD
    printf("=== callback_test [pre-fix hw_jpeg_decoder.c -- must FAIL] ===\n");
#else
    printf("=== callback_test [current hw_jpeg_decoder.c -- must PASS] ===\n");
#endif

    fakejpeg_reset();
    fakejpeg_configure(8, 8, JPEG_444_SUBSAMPLING);
    fakejpeg_fill(g_padded_src, sizeof g_padded_src, 256);  /* true JPEG length: 256 bytes */
    fakejpeg_fill(g_trunc_src, sizeof g_trunc_src, 0);      /* no marker anywhere */

    expect("JPEG_DecodeToFrameInit", JPEG_DecodeToFrameInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);
    uint32_t w, h;

    printf("\nvideo: SrcSize == true length, on every good frame (the caller this bug\n"
           "killed) -- the callback must fire (proves the exact-length path is really\n"
           "exercised) and the frame must still decode:\n\n");
    fakejpeg_reset_counters();
    expect("video frame, exact length, decodes",
           JPEG_DecodeToFrame(fakejpeg_addr(g_padded_src), 256, fakejpeg_addr(g_dest), 0, 0, 0xFF), 0);
    expect("  ...and the end-of-input callback fired", fakejpeg_get_data_callback_count() > 0, 1);

    printf("\ncovers and .gw artwork: SrcSize is padded, so the callback never fires --\n"
           "this is why testing them \"proved\" the decoder was fine while video was dead:\n\n");
    fakejpeg_reset_counters();
    expect("cover, cache-slot-sized SrcSize, decodes",
           JPEG_DecodeToBuffer(fakejpeg_addr(g_padded_src), 4096, fakejpeg_addr(g_dest), &w, &h, 0xFF), 0);
    expect("  ...and the callback did NOT fire", fakejpeg_get_data_callback_count(), 0);

    fakejpeg_reset_counters();
    expect(".gw artwork, rest-of-file-sized SrcSize, decodes",
           JPEG_DecodeToBuffer(fakejpeg_addr(g_padded_src), sizeof g_padded_src, fakejpeg_addr(g_dest), &w, &h, 0xFF), 0);
    expect("  ...and the callback did NOT fire", fakejpeg_get_data_callback_count(), 0);

    printf("\na frame that really is truncated must still be rejected -- the fix must\n"
           "not turn a genuine error into a false decode:\n\n");
    expect("truncated frame (no EOI in what it was given), rejected",
           JPEG_DecodeToFrame(fakejpeg_addr(g_trunc_src), sizeof g_trunc_src, fakejpeg_addr(g_dest), 0, 0, 0xFF), 1);

    printf("\n%s\n\n", g_failures
           ? "FAIL"
           : "PASS: exact-length callers hit the end-of-input callback on every good"
             " frame and still decode; padded callers never hit it; a real truncation"
             " is still rejected");
    return g_failures ? 1 : 0;
}
