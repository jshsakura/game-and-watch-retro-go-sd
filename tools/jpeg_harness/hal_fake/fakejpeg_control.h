#ifndef FAKEJPEG_CONTROL_H
#define FAKEJPEG_CONTROL_H
#include <stdint.h>

/* Test-only hooks into hal_jpeg_fake.c. Nothing in hw_jpeg_decoder.c calls
 * these -- they exist so a test can drive the fake HAL's two unmodeled
 * inputs: what the (silicon) header parser would have reported, and a fault
 * precondition production only ever observed as a symptom (hal=2 err=0
 * rej=0 on every frame), never as a diagnosed C-level cause. */

/* Real HAL_JPEG_GetInfo (stm32h7xx_hal_jpeg.c:1290-1355) decodes width/
 * height/subsampling out of hardware registers the peripheral wrote while
 * parsing the JPEG header -- there is no C logic to transcribe, only a
 * register layout a host doesn't have. Call before the decode under test;
 * the fake's one HAL_JPEG_InfoReadyCallback call per decode uses these. */
void fakejpeg_configure(uint32_t width, uint32_t height, uint32_t subsampling);
void fakejpeg_reset(void);

/* Forces the JPEG handle captured by the most recent HAL_JPEG_Init/Decode
 * call into Lock=HAL_LOCKED, State=HAL_JPEG_STATE_READY -- the precondition
 * lock_test.c exercises. How a real handle would get left this way outside
 * a normal HAL_JPEG_Decode call (which always unlocks on every return path)
 * was never pinned down on the device; what IS real and load-bearing is the
 * driver's response to it, which this drives unmodified. Must be called
 * after at least one real HAL_JPEG_Init/Decode call has run (so a handle
 * has been captured). */
void fakejpeg_inject_stuck_lock(void);

/* hw_jpeg_decoder.c's own HAL_JPEG_GetDataCallback has no counter of its own
 * (it is shipped device code -- not ours to instrument), so the fake counts
 * calls at the one place it is faithful to call it from, JPEG_ReadInputData's
 * "InDataLength == JpegInCount" branch (hal_jpeg_fake.c). This is the direct
 * observable of the bug callback_test.c exists to pin: does the exact-length
 * caller reach the same "give me more" request the real device did. */
uint32_t fakejpeg_get_data_callback_count(void);
void fakejpeg_reset_counters(void);

/* Real HAL_JPEG_GetInfo returns HAL_ERROR on a header that doesn't decode to
 * a recognised colorspace (hal_jpeg.c:1315-1318) -- there is no register
 * encoding in this fake to reproduce that honestly, so this forces the NEXT
 * HAL_JPEG_GetInfo call to fail once, exercising hw_jpeg_decoder.c's own
 * response (HAL_JPEG_InfoReadyCallback's decode_rejected/g_jpeg_rej=1 path). */
void fakejpeg_force_getinfo_failure(void);

#endif
