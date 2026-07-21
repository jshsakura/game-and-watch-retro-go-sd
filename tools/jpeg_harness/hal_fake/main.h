#ifndef HAL_FAKE_MAIN_H
#define HAL_FAKE_MAIN_H
/* Host stand-in for Core/Inc/main.h, which on the device pulls in the full
 * STM32H7 HAL. hw_jpeg_decoder.c #includes "main.h" for exactly five things:
 * the JPEG/DMA2D handle+config types, the JPEG/DMA2D instance macros, the
 * HAL_JPEG_ and HAL_DMA2D_ entry points it calls, and SCB_Clean*Cache(). This
 * header supplies host versions of all five so hw_jpeg_decoder.c compiles
 * and links completely unmodified. Types and values are transcribed from:
 *
 *   Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_def.h   (HAL_StatusTypeDef, HAL_LockTypeDef)
 *   Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_jpeg.h  (JPEG_* types/values)
 *   Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_dma2d.h (DMA2D_* types/values)
 *
 * The behavioural half of the fake -- what HAL_JPEG_Init/Decode/Abort
 * actually DO -- lives in hal_jpeg_fake.c, transcribed from
 * stm32h7xx_hal_jpeg.c with line citations at each site.
 */

#include <stdint.h>

/* ---- stm32h7xx_hal_def.h ------------------------------------------------ */
typedef enum { HAL_OK = 0x00, HAL_ERROR = 0x01, HAL_BUSY = 0x02, HAL_TIMEOUT = 0x03 } HAL_StatusTypeDef;
typedef enum { HAL_UNLOCKED = 0x00, HAL_LOCKED = 0x01 } HAL_LockTypeDef;
#define HAL_MAX_DELAY 0xFFFFFFFFU

/* ---- stm32h7xx_hal_jpeg.h ------------------------------------------------ */
typedef struct {
    uint32_t ColorSpace;
    uint32_t ChromaSubsampling;
    uint32_t ImageHeight;
    uint32_t ImageWidth;
    uint32_t ImageQuality;
} JPEG_ConfTypeDef;

typedef enum {
    HAL_JPEG_STATE_RESET         = 0x00U,
    HAL_JPEG_STATE_READY         = 0x01U,
    HAL_JPEG_STATE_BUSY          = 0x02U,
    HAL_JPEG_STATE_BUSY_ENCODING = 0x03U,
    HAL_JPEG_STATE_BUSY_DECODING = 0x04U,
    HAL_JPEG_STATE_TIMEOUT       = 0x05U,
    HAL_JPEG_STATE_ERROR         = 0x06U,
} HAL_JPEG_STATETypeDef;

/* Register block. The fake never reads/writes real registers -- everything
 * it needs is tracked in JPEG_HandleTypeDef fields instead (see
 * hal_jpeg_fake.c). Instance only ever gets assigned/compared as a pointer
 * value by hw_jpeg_decoder.c, never dereferenced. */
typedef struct { int unused; } JPEG_TypeDef;
#define JPEG ((JPEG_TypeDef *)0)

typedef struct {
    JPEG_TypeDef           *Instance;
    JPEG_ConfTypeDef         Conf;
    uint8_t                 *pJpegInBuffPtr;
    uint8_t                 *pJpegOutBuffPtr;
    uint32_t                 JpegInCount;
    uint32_t                 JpegOutCount;
    uint32_t                 InDataLength;
    uint32_t                 OutDataLength;
    uint8_t                  CustomQuanTable;
    uint8_t                  *QuantTable0;
    uint8_t                  *QuantTable1;
    uint8_t                  *QuantTable2;
    uint8_t                  *QuantTable3;
    HAL_LockTypeDef           Lock;
    HAL_JPEG_STATETypeDef     State;
    uint32_t                  ErrorCode;
    uint32_t                  Context;
} JPEG_HandleTypeDef;

#define HAL_JPEG_ERROR_NONE     ((uint32_t)0x00000000U)
#define HAL_JPEG_ERROR_TIMEOUT  ((uint32_t)0x00000008U)

#define JPEG_444_SUBSAMPLING  ((uint32_t)0x00000000U)
#define JPEG_420_SUBSAMPLING  ((uint32_t)0x00000001U)
#define JPEG_422_SUBSAMPLING  ((uint32_t)0x00000002U)

#define JPEG_PAUSE_RESUME_INPUT         ((uint32_t)0x00000001U)
#define JPEG_PAUSE_RESUME_OUTPUT        ((uint32_t)0x00000002U)
#define JPEG_PAUSE_RESUME_INPUT_OUTPUT  ((uint32_t)0x00000003U)

/* stm32h7xx_hal_jpeg.c:261-262 (JPEG_Private_Constants) -- context bits that
 * gate JPEG_Process's input/output servicing while paused. Needed so the
 * fake's decode loop reacts to HAL_JPEG_Pause the same way JPEG_Process does. */
#define JPEG_CONTEXT_PAUSE_INPUT   ((uint32_t)0x00001000U)
#define JPEG_CONTEXT_PAUSE_OUTPUT  ((uint32_t)0x00002000U)

/* stm32h7xx_hal_jpeg.c:243-245 -- real input-FIFO service chunk size (words).
 * The fake feeds input in chunks of this size so a padded caller's slack
 * genuinely spans multiple polls, same as on the device, instead of being
 * consumed in one unrealistic instantaneous step (see hal_jpeg_fake.c). */
#define JPEG_FIFO_TH_SIZE ((uint32_t)8U)

HAL_StatusTypeDef HAL_JPEG_Init(JPEG_HandleTypeDef *hjpeg);
HAL_StatusTypeDef HAL_JPEG_DeInit(JPEG_HandleTypeDef *hjpeg);
HAL_StatusTypeDef HAL_JPEG_Decode(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataIn, uint32_t InDataLength,
                                   uint8_t *pDataOutMCU, uint32_t OutDataLength, uint32_t Timeout);
HAL_StatusTypeDef HAL_JPEG_Abort(JPEG_HandleTypeDef *hjpeg);
HAL_StatusTypeDef HAL_JPEG_Pause(JPEG_HandleTypeDef *hjpeg, uint32_t XferSelection);
HAL_StatusTypeDef HAL_JPEG_GetInfo(JPEG_HandleTypeDef *hjpeg, JPEG_ConfTypeDef *pInfo);
void HAL_JPEG_ConfigInputBuffer(JPEG_HandleTypeDef *hjpeg, uint8_t *pNewInputBuffer, uint32_t InDataLength);

/* Callbacks hw_jpeg_decoder.c defines and the fake HAL calls back into by
 * name, exactly like the real driver does in its
 * USE_HAL_JPEG_REGISTER_CALLBACKS==0 build (stm32h7xx_hal_jpeg.c:3337-3348,
 * :3419-3423, :3586-3591, :3440-3447 -- direct calls, not through a
 * hjpeg->XCallback pointer). */
void HAL_JPEG_InfoReadyCallback(JPEG_HandleTypeDef *hjpeg, JPEG_ConfTypeDef *pInfo);
void HAL_JPEG_GetDataCallback(JPEG_HandleTypeDef *hjpeg, uint32_t NbDecodedData);
void HAL_JPEG_DecodeCpltCallback(JPEG_HandleTypeDef *hjpeg);
void HAL_JPEG_ErrorCallback(JPEG_HandleTypeDef *hjpeg);
void HAL_JPEG_DataReadyCallback(JPEG_HandleTypeDef *hjpeg, uint8_t *pDataOut, uint32_t OutDataLength);

/* ---- stm32h7xx_hal_dma2d.h ------------------------------------------------
 * hw_jpeg_decoder.c uses DMA2D only to blit the decoded YCbCr MCU buffer to
 * RGB565. Pixel content is out of scope for a lock/callback/floor regression
 * test (none of the three shipped bugs were in this path), so every entry
 * point below just reports success; field names/shapes still match the real
 * struct so hw_jpeg_decoder.c's field assignments compile unmodified. */
typedef struct { int unused; } DMA2D_TypeDef;
#define DMA2D ((DMA2D_TypeDef *)0)

typedef struct {
    uint32_t Mode, ColorMode, OutputOffset, AlphaInverted, RedBlueSwap, LineOffsetMode;
} DMA2D_InitTypeDef;

typedef struct {
    uint32_t AlphaMode, InputAlpha, InputColorMode, ChromaSubSampling, InputOffset, RedBlueSwap, AlphaInverted;
} DMA2D_LayerCfgTypeDef;

typedef struct {
    DMA2D_TypeDef          *Instance;
    DMA2D_InitTypeDef       Init;
    DMA2D_LayerCfgTypeDef   LayerCfg[2];
    void                    *XferCpltCallback; /* only ever assigned NULL by hw_jpeg_decoder.c */
} DMA2D_HandleTypeDef;

#define DMA2D_M2M_BLEND_BG     ((uint32_t)0x00020000U)
#define DMA2D_OUTPUT_RGB565    ((uint32_t)0x00000002U)
#define DMA2D_REGULAR_ALPHA    ((uint32_t)0x00000000U)
#define DMA2D_RB_REGULAR       ((uint32_t)0x00000000U)
#define DMA2D_LOM_PIXELS       ((uint32_t)0x00000000U)
#define DMA2D_REPLACE_ALPHA    ((uint32_t)0x00000001U)
#define DMA2D_INPUT_YCBCR      ((uint32_t)0x00000002U)
#define DMA2D_INPUT_A8         ((uint32_t)0x00000009U)
#define DMA2D_CSS_420          ((uint32_t)0x00000000U)
#define DMA2D_CSS_422          ((uint32_t)0x00000001U)
#define DMA2D_NO_CSS           ((uint32_t)0x00000000U)

HAL_StatusTypeDef HAL_DMA2D_Init(DMA2D_HandleTypeDef *hdma2d);
HAL_StatusTypeDef HAL_DMA2D_ConfigLayer(DMA2D_HandleTypeDef *hdma2d, uint32_t LayerIdx);
HAL_StatusTypeDef HAL_DMA2D_Start(DMA2D_HandleTypeDef *hdma2d, uint32_t pdata, uint32_t DstAddress,
                                   uint32_t Width, uint32_t Height);
HAL_StatusTypeDef HAL_DMA2D_PollForTransfer(DMA2D_HandleTypeDef *hdma2d, uint32_t Timeout);
HAL_StatusTypeDef HAL_DMA2D_DeInit(DMA2D_HandleTypeDef *hdma2d);

void SCB_CleanDCache(void);
void SCB_CleanInvalidateDCache(void);

#endif /* HAL_FAKE_MAIN_H */
