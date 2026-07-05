#ifndef STUB_MAIN_H
#define STUB_MAIN_H
#include <stdint.h>
#include <stdbool.h>
typedef int SAI_HandleTypeDef; typedef int DMA_HandleTypeDef;
void HAL_Delay(uint32_t ms); uint32_t HAL_GetTick(void);
void wdog_refresh(void);
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef*, uint8_t*, uint16_t);
int HAL_SAI_DMAStop(SAI_HandleTypeDef*);
#endif
