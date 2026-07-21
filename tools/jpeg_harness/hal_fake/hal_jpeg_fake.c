/* Host fake of the ST HAL JPEG/DMA2D API hw_jpeg_decoder.c is written
 * against. Every line that touches Lock, State, ErrorCode, InDataLength's
 * floor-to-4, or when a callback fires is transcribed from
 * Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_jpeg.c with a citation --
 * that is the real driver's control flow, not a simplification of it.
 *
 * What genuinely cannot exist on a host is the JPEG core's own silicon:
 * Huffman/DCT decode and the header-parse hardware that fills in
 * CONFR1/CONFR3-6. Those are replaced by two documented stand-ins:
 *
 *   - fakejpeg_configure() supplies what HAL_JPEG_GetInfo would have read
 *     back from those registers (see fakejpeg_control.h).
 *   - "has the decode core reached the image's End-Of-Image" is decided by
 *     scanning the input buffer for the first FF D9 marker, the C-level
 *     observable of the same hardware event real firmware relies on.
 *
 * Everything else below -- the lock check, the two-site-gated Init unlock,
 * the %4 floor, the FIFO-threshold-chunked feed loop and when it fires
 * GetDataCallback, what Abort leaves behind -- is the real state machine.
 */
#include "main.h"
#include "fakejpeg_control.h"
#include <stddef.h>

static uint32_t g_fake_width = 16, g_fake_height = 16, g_fake_subsampling = JPEG_420_SUBSAMPLING;
static JPEG_HandleTypeDef *g_captured_hjpeg;
static uint32_t g_get_data_callback_calls;

uint32_t fakejpeg_get_data_callback_count(void) { return g_get_data_callback_calls; }
void fakejpeg_reset_counters(void) { g_get_data_callback_calls = 0; }

void fakejpeg_configure(uint32_t width, uint32_t height, uint32_t subsampling)
{
    g_fake_width = width;
    g_fake_height = height;
    g_fake_subsampling = subsampling;
}

void fakejpeg_reset(void)
{
    g_fake_width = 16;
    g_fake_height = 16;
    g_fake_subsampling = JPEG_420_SUBSAMPLING;
}

void fakejpeg_inject_stuck_lock(void)
{
    /* Applied immediately, onto whichever handle the most recent
     * HAL_JPEG_Init/HAL_JPEG_Decode call was given (captured below) -- NOT
     * deferred to the next such call. Deferring it would land the injection
     * AFTER the fix's own JPEG_HandleReset() has already run (it executes
     * before HAL_JPEG_Init is even called), silently wiping it out again and
     * making the fixed code look broken too. Can't poke JPEG_Handle
     * directly; it is static inside hw_jpeg_decoder.c. */
    if (g_captured_hjpeg != NULL) {
        g_captured_hjpeg->State = HAL_JPEG_STATE_READY;
        g_captured_hjpeg->Lock = HAL_LOCKED;
    }
}

static void capture(JPEG_HandleTypeDef *hjpeg) { g_captured_hjpeg = hjpeg; }

/* HAL_GetTick() stand-in: a free-running counter advanced once per poll
 * iteration inside HAL_JPEG_Decode, not wall-clock time. Real timeout math
 * (stm32h7xx_hal_jpeg.c:1672-1686) is (HAL_GetTick() - tickstart) > Timeout;
 * a counter instead of real milliseconds means a genuinely-truncated-input
 * test resolves in microseconds instead of sleeping JPEG_DECODE_TIMEOUT_MS. */
static uint32_t g_fake_tick;
static uint32_t fake_tick(void) { return ++g_fake_tick; }

/* First FF D9 (End Of Image) at or after `from`, strictly inside [0, limit).
 * Returns the offset one past the marker (bytes needed to include the whole
 * image), or 0 if none is found in range -- 0 also doubles as "not found"
 * since a real EOI can never end at offset 0. */
static uint32_t find_eoi(const uint8_t *buf, uint32_t from, uint32_t limit)
{
    if (buf == NULL || limit < 2) return 0;
    for (uint32_t i = from; i + 1 < limit; i++) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9) return i + 2;
    }
    return 0;
}

/* stm32h7xx_hal_jpeg.c:484-600, HAL_JPEG_Init. The fact under test: Lock is
 * cleared ONLY inside `if (State == RESET)` (:538-541 in the
 * USE_HAL_JPEG_REGISTER_CALLBACKS==0 build this project uses); State,
 * ErrorCode and Context are reset unconditionally every call regardless. */
HAL_StatusTypeDef HAL_JPEG_Init(JPEG_HandleTypeDef *hjpeg)
{
    if (hjpeg == NULL) return HAL_ERROR;
    capture(hjpeg);

    if (hjpeg->State == HAL_JPEG_STATE_RESET) {
        hjpeg->Lock = HAL_UNLOCKED;                  /* :541, gated */
    }
    hjpeg->JpegInCount = 0;
    hjpeg->JpegOutCount = 0;
    hjpeg->State = HAL_JPEG_STATE_READY;              /* :590 */
    hjpeg->ErrorCode = HAL_JPEG_ERROR_NONE;            /* :593 -- destroys the evidence */
    hjpeg->Context = 0;                                /* :596 */
    return HAL_OK;
}

/* stm32h7xx_hal_jpeg.c:608-651, HAL_JPEG_DeInit. */
HAL_StatusTypeDef HAL_JPEG_DeInit(JPEG_HandleTypeDef *hjpeg)
{
    if (hjpeg == NULL) return HAL_ERROR;
    hjpeg->State = HAL_JPEG_STATE_BUSY;
    hjpeg->ErrorCode = HAL_JPEG_ERROR_NONE;
    hjpeg->JpegInCount = 0;
    hjpeg->JpegOutCount = 0;
    hjpeg->State = HAL_JPEG_STATE_RESET;
    hjpeg->Context = 0;
    hjpeg->Lock = HAL_UNLOCKED;                        /* :647 */
    return HAL_OK;
}

/* stm32h7xx_hal_jpeg.c:1625-1707, HAL_JPEG_Decode (polling). */
HAL_StatusTypeDef HAL_JPEG_Decode(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataIn, uint32_t InDataLength,
                                   uint8_t *pDataOutMCU, uint32_t OutDataLength, uint32_t Timeout)
{
    if (hjpeg == NULL || pDataIn == NULL || pDataOutMCU == NULL) return HAL_ERROR;
    capture(hjpeg);

    if (hjpeg->Lock == HAL_LOCKED) return HAL_BUSY;    /* __HAL_LOCK, :1641 -- the whole bug */
    hjpeg->Lock = HAL_LOCKED;

    uint32_t tickstart = fake_tick();

    if (hjpeg->State != HAL_JPEG_STATE_READY) {
        hjpeg->Lock = HAL_UNLOCKED;                    /* :1698-1703 */
        return HAL_BUSY;
    }

    hjpeg->State = HAL_JPEG_STATE_BUSY_DECODING;       /* :1649 */
    hjpeg->pJpegInBuffPtr = pDataIn;
    hjpeg->pJpegOutBuffPtr = pDataOutMCU;
    hjpeg->InDataLength = InDataLength - (InDataLength % 4UL);     /* :1659 -- THE FLOOR */
    hjpeg->OutDataLength = OutDataLength - (OutDataLength % 4UL);  /* :1660 */
    hjpeg->JpegInCount = 0;
    hjpeg->JpegOutCount = 0;

    uint32_t true_len = find_eoi(pDataIn, 0, hjpeg->InDataLength);
    int info_fired = 0;
    uint32_t total_fed = 0, decoded_through = 0;

    for (;;) {
        if (!info_fired) {
            info_fired = 1;
            /* JPEG_Process:3332-3348 -- header-parse-complete callback,
             * fired once, before any input/output FIFO servicing. */
            hjpeg->Conf.ImageWidth = g_fake_width;
            hjpeg->Conf.ImageHeight = g_fake_height;
            hjpeg->Conf.ChromaSubsampling = g_fake_subsampling;
            hjpeg->Conf.ImageQuality = 0;
            HAL_JPEG_InfoReadyCallback(hjpeg, &hjpeg->Conf);
        }

        /* Input FIFO handling -- JPEG_ReadInputData, :3567-3599, chunked at
         * the real FIFO threshold (JPEG_FIFO_TH_SIZE words) so a padded
         * caller's slack genuinely spans many polls before ever reaching
         * InDataLength, same as on the device. */
        if ((hjpeg->Context & JPEG_CONTEXT_PAUSE_INPUT) == 0) {
            if (hjpeg->InDataLength == 0UL) {
                /* :3575-3579 -- nothing left to feed; nothing to do. */
            } else if (hjpeg->InDataLength > hjpeg->JpegInCount) {
                uint32_t chunk = hjpeg->InDataLength - hjpeg->JpegInCount;
                uint32_t th = JPEG_FIFO_TH_SIZE * 4UL;
                if (chunk > th) chunk = th;
                hjpeg->JpegInCount += chunk;
                total_fed += chunk;
            } else { /* InDataLength == JpegInCount, :3584-3599 */
                g_get_data_callback_calls++;
                HAL_JPEG_GetDataCallback(hjpeg, hjpeg->JpegInCount);
                if (hjpeg->InDataLength > 4UL)
                    hjpeg->InDataLength -= hjpeg->InDataLength % 4UL;  /* :3595 */
                hjpeg->JpegInCount = 0;
            }
        }

        /* EOC handling -- JPEG_Process:3400-3457. The decode core trails
         * what has been pushed into the input FIFO by about one poll (using
         * the PREVIOUS iteration's total_fed, not this one's, models that
         * lag); that lag is what makes an exact-length caller's
         * GetDataCallback fire a poll before completion instead of never. */
        if (true_len != 0 && decoded_through >= true_len &&
            (hjpeg->Context & JPEG_CONTEXT_PAUSE_OUTPUT) == 0) {
            /* JPEG_Process:3416-3426 -- DataReadyCallback fires once more,
             * with whatever is still sitting in JpegOutCount, right before
             * DecodeCpltCallback. Real JpegOutCount tracks bytes actually
             * streamed out of the output FIFO, which this fake never moves
             * (see main.h's DMA2D note -- pixel content is out of scope);
             * standing in with a nonzero placeholder is enough to exercise
             * the real "was there anything to flush" branch honestly. */
            hjpeg->JpegOutCount = 4;
            HAL_JPEG_DataReadyCallback(hjpeg, hjpeg->pJpegOutBuffPtr, hjpeg->JpegOutCount);
            hjpeg->JpegOutCount = 0;

            hjpeg->State = HAL_JPEG_STATE_READY;       /* :3437 */
            hjpeg->Lock = HAL_UNLOCKED;                /* :3434 */
            HAL_JPEG_DecodeCpltCallback(hjpeg);         /* :3445 */
            return HAL_OK;
        }
        decoded_through = total_fed;

        if (Timeout != HAL_MAX_DELAY && (fake_tick() - tickstart) > Timeout) {
            hjpeg->ErrorCode |= HAL_JPEG_ERROR_TIMEOUT; /* :1678 */
            hjpeg->Lock = HAL_UNLOCKED;                 /* :1681 */
            hjpeg->State = HAL_JPEG_STATE_READY;        /* :1684 */
            return HAL_TIMEOUT;
        }
    }
}

/* stm32h7xx_hal_jpeg.c:2199-2285, HAL_JPEG_Abort. No __HAL_LOCK at entry in
 * the real driver either -- by the time JPEG_Run calls this, HAL_JPEG_Decode
 * has already unlocked on every return path. The COF-wait loop (:2235-2247)
 * polls a hardware "codec still running" flag with no host equivalent;
 * modeled as already-clear (the only place this function diverges from a
 * literal transcription). The fact under test is the tail: ErrorCode!=NONE
 * leaves State=ERROR, full stop, regardless of what caused it. */
HAL_StatusTypeDef HAL_JPEG_Abort(JPEG_HandleTypeDef *hjpeg)
{
    if (hjpeg == NULL) return HAL_ERROR;

    hjpeg->Context &= ~(JPEG_CONTEXT_PAUSE_INPUT | JPEG_CONTEXT_PAUSE_OUTPUT);  /* :2265 */
    hjpeg->JpegInCount = 0;                            /* :2261 */
    hjpeg->JpegOutCount = 0;                            /* :2262 */

    if (hjpeg->ErrorCode != HAL_JPEG_ERROR_NONE) {
        hjpeg->State = HAL_JPEG_STATE_ERROR;            /* :2270 -- the trap JPEG_Run works around */
        hjpeg->Lock = HAL_UNLOCKED;                      /* :2272 */
        return HAL_ERROR;
    }
    hjpeg->State = HAL_JPEG_STATE_READY;                /* :2278 */
    hjpeg->Lock = HAL_UNLOCKED;                          /* :2280 */
    return HAL_OK;
}

/* stm32h7xx_hal_jpeg.c:2100-2163 (polling branch), HAL_JPEG_Pause. */
HAL_StatusTypeDef HAL_JPEG_Pause(JPEG_HandleTypeDef *hjpeg, uint32_t XferSelection)
{
    if (hjpeg == NULL) return HAL_ERROR;
    if ((XferSelection & JPEG_PAUSE_RESUME_INPUT) == JPEG_PAUSE_RESUME_INPUT)
        hjpeg->Context |= JPEG_CONTEXT_PAUSE_INPUT;
    if ((XferSelection & JPEG_PAUSE_RESUME_OUTPUT) == JPEG_PAUSE_RESUME_OUTPUT)
        hjpeg->Context |= JPEG_CONTEXT_PAUSE_OUTPUT;
    return HAL_OK;
}

/* stm32h7xx_hal_jpeg.c:1290-1355, HAL_JPEG_GetInfo. The real function decodes
 * pInfo out of hardware registers (CONFR1/CONFR3/CONFR4-6) the peripheral
 * wrote while parsing the header -- there is no C logic to transcribe, only
 * a register layout a host doesn't have. hjpeg->Conf was already populated
 * by HAL_JPEG_Decode's own JPEG_Process-equivalent step right before it
 * called InfoReadyCallback (mirroring the real JPEG_Process:3337 call to
 * this same function), so reading it back here is faithful to what real
 * GetInfo would return for this image. */
static int g_force_getinfo_failure;

/* Real HAL_JPEG_GetInfo returns HAL_ERROR when the header-parse registers
 * don't decode to any of the three recognised colorspace bit patterns
 * (hal_jpeg.c:1315-1318, "else return HAL_ERROR") -- a malformed/corrupt
 * header, which this fake has no register encoding to reproduce honestly.
 * Forcing the NEXT call to fail once is the documented stand-in for that,
 * exercising hw_jpeg_decoder.c's own response to it (HAL_JPEG_InfoReadyCallback's
 * "decode_rejected=1, g_jpeg_rej=1" branch). */
void fakejpeg_force_getinfo_failure(void) { g_force_getinfo_failure = 1; }

HAL_StatusTypeDef HAL_JPEG_GetInfo(JPEG_HandleTypeDef *hjpeg, JPEG_ConfTypeDef *pInfo)
{
    if (hjpeg == NULL || pInfo == NULL) return HAL_ERROR;
    if (g_force_getinfo_failure) {
        g_force_getinfo_failure = 0;
        return HAL_ERROR;
    }
    *pInfo = hjpeg->Conf;
    return HAL_OK;
}

/* stm32h7xx_hal_jpeg.c:2173-2177, HAL_JPEG_ConfigInputBuffer, verbatim --
 * no lock, no validation, just the two field writes. */
void HAL_JPEG_ConfigInputBuffer(JPEG_HandleTypeDef *hjpeg, uint8_t *pNewInputBuffer, uint32_t InDataLength)
{
    if (hjpeg == NULL) return;
    hjpeg->pJpegInBuffPtr = pNewInputBuffer;
    hjpeg->InDataLength = InDataLength;
}

/* ---- DMA2D + cache: no shipped bug lived here, see main.h's note. ---- */
HAL_StatusTypeDef HAL_DMA2D_Init(DMA2D_HandleTypeDef *hdma2d) { (void)hdma2d; return HAL_OK; }
HAL_StatusTypeDef HAL_DMA2D_ConfigLayer(DMA2D_HandleTypeDef *hdma2d, uint32_t LayerIdx)
{ (void)hdma2d; (void)LayerIdx; return HAL_OK; }
HAL_StatusTypeDef HAL_DMA2D_Start(DMA2D_HandleTypeDef *hdma2d, uint32_t pdata, uint32_t DstAddress,
                                   uint32_t Width, uint32_t Height)
{ (void)hdma2d; (void)pdata; (void)DstAddress; (void)Width; (void)Height; return HAL_OK; }
HAL_StatusTypeDef HAL_DMA2D_PollForTransfer(DMA2D_HandleTypeDef *hdma2d, uint32_t Timeout)
{ (void)hdma2d; (void)Timeout; return HAL_OK; }
HAL_StatusTypeDef HAL_DMA2D_DeInit(DMA2D_HandleTypeDef *hdma2d) { (void)hdma2d; return HAL_OK; }

void SCB_CleanDCache(void) {}
void SCB_CleanInvalidateDCache(void) {}
