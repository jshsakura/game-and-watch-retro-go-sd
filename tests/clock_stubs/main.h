#ifndef STUB_MAIN_H
#define STUB_MAIN_H
#include <stdint.h>
#include <stdbool.h>
typedef int SAI_HandleTypeDef; typedef int DMA_HandleTypeDef;
/* mirrors Core/Inc/main.h: places a function into the always-resident
 * intflash section. A no-op attribute on the host, but rg_clock.c's extern
 * decl of rg_main.c's main_menu_timeout_cb (shared PAUSE-menu row) must parse
 * the same way here as it does in the firmware build. */
#define GLOBAL_DATA __attribute__((section(".intflash_global")))
void HAL_Delay(uint32_t ms); uint32_t HAL_GetTick(void);
void wdog_refresh(void);
int HAL_SAI_Transmit_DMA(SAI_HandleTypeDef*, uint8_t*, uint16_t);
int HAL_SAI_DMAStop(SAI_HandleTypeDef*);
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize);  /* CMSIS cache op (MP3-alarm staging) */
#endif
