/* Host-test stub for main.h — just the pieces video_decode.c/video_play.c/
 * video_audio.c need: tick source, watchdog kick, the CMSIS cache op the JPEG
 * decoder path uses before handing SrcAddress to the HW peripheral. */
#ifndef STUB_MAIN_H
#define STUB_MAIN_H
#include <stdint.h>
#include <stdbool.h>
typedef int SAI_HandleTypeDef; typedef int DMA_HandleTypeDef;
void HAL_Delay(uint32_t ms); uint32_t HAL_GetTick(void);
void wdog_refresh(void);
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef*, uint8_t*, uint16_t);
int HAL_SAI_DMAStop(SAI_HandleTypeDef*);
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize);
#endif
