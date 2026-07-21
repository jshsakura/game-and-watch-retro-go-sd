/* Why "dec=14 v=272" meant nine frames out of ten rejected, not late -- and
 * why rounding UP is the fix, not rounding down, not leaving it alone.
 * Drives the REAL JPEG_DecodeToFrame against the faithful HAL fake in
 * tools/jpeg_harness/hal_fake (see lock_test.c's header for how that fake is
 * built and cited).
 *
 * HAL floors whatever length it is given to a multiple of 4 before it ever
 * touches the peripheral (stm32h7xx_hal_jpeg.c:1659, HAL_JPEG_Decode):
 *
 *     hjpeg->InDataLength = InDataLength - (InDataLength % 4UL);
 *
 * A JPEG's last two bytes are the end-of-image marker, FF D9. If the
 * peripheral never receives them it never reaches EOC, and
 * HAL_JPEG_Decode's own timeout is the only way out (:1672-1686,
 * HAL_TIMEOUT). A 5,949-byte frame handed over as exactly 5,949 is floored
 * to 5,948: the last byte of FF D9 never arrives. Only frames whose true
 * length was already a multiple of 4 survived -- roughly 1 in 4, by
 * rounding luck.
 *
 * The bug was JPEG_Run (hw_jpeg_decoder.c) passing SrcSize straight through.
 * The fix rounds UP instead -- (SrcSize + 3) & ~3, hw_jpeg_decoder.c:131 --
 * so HAL's own floor lands back on a size that still contains the true
 * length. The extra padding bytes are still inside the caller's buffer and
 * are never read, because the decoder stops at EOI regardless of how much
 * more it was handed.
 */
#include <stdio.h>
#include <stdint.h>

#include "main.h"              /* JPEG_444_SUBSAMPLING etc. -- fake HAL constants */
#include "hw_jpeg_decoder.h"
#include "fakejpeg_control.h"
#include "fakejpeg_buf.h"

static uint8_t g_work_buf[4096];
static uint8_t g_dest[4096];
static uint8_t g_buf_misaligned[5952]; /* true image: 5949 bytes, not a multiple of 4 */
static uint8_t g_buf_aligned[5948];    /* true image: 5948 bytes, already a multiple of 4 */

static int g_failures = 0;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) printf("  OK   %-58s -> %u\n", what, got);
    else { printf("  FAIL %-58s -> got %u, want %u\n", what, got, want); g_failures++; }
}

int main(void)
{
#ifdef PRE_FIX_BUILD
    printf("=== floor_test [pre-fix hw_jpeg_decoder.c -- must FAIL] ===\n");
#else
    printf("=== floor_test [current hw_jpeg_decoder.c -- must PASS] ===\n");
#endif

    fakejpeg_reset();
    fakejpeg_configure(8, 8, JPEG_444_SUBSAMPLING);
    fakejpeg_fill(g_buf_misaligned, sizeof g_buf_misaligned, 5949);
    fakejpeg_fill(g_buf_aligned, sizeof g_buf_aligned, 5948);

    expect("JPEG_DecodeToFrameInit", JPEG_DecodeToFrameInit(fakejpeg_addr(g_work_buf), sizeof g_work_buf), 0);

    printf("\nreal frame from the device HUD: 5949-byte, misaligned true length --\n"
           "HAL floors whatever it's handed to a multiple of 4, so an exact-length\n"
           "caller must be decodable regardless:\n\n");
    expect("5949-byte frame, exact length",
           JPEG_DecodeToFrame(fakejpeg_addr(g_buf_misaligned), 5949, fakejpeg_addr(g_dest), 0, 0, 0xFF), 0);

    printf("\na length that was ALREADY a multiple of 4 -- the lucky slice that always\n"
           "worked, pre-fix and post-fix alike:\n\n");
    expect("5948-byte frame, aligned length",
           JPEG_DecodeToFrame(fakejpeg_addr(g_buf_aligned), 5948, fakejpeg_addr(g_dest), 0, 0, 0xFF), 0);

    printf("\na genuinely truncated frame (missing the real EOI, not just floored to it)\n"
           "must still be rejected -- the fix must not paper over a real truncation:\n\n");
    expect("true image is 5949 bytes, only 5900 ever arrive",
           JPEG_DecodeToFrame(fakejpeg_addr(g_buf_misaligned), 5900, fakejpeg_addr(g_dest), 0, 0, 0xFF), 1);

    printf("\n%s\n\n", g_failures
           ? "FAIL"
           : "PASS: an exact-length frame survives HAL's floor-to-4, and a real"
             " truncation is still caught");
    return g_failures ? 1 : 0;
}
