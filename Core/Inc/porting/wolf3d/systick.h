#ifndef _WOLF_SYSTICK_H_
#define _WOLF_SYSTICK_H_

/* G&W port shim for the Wolf3D-STM32 driver header (src/drivers/systick.h).
 * delay_ms()/get_ticks_ms() are implemented in main_wolf3d.c over the HAL tick;
 * systick_setup() is unused here (the firmware owns SysTick). */
#include <stdint.h>

void systick_setup(void);
void delay_ms(uint32_t ms);
uint32_t get_ticks_ms(void);

#endif /* _WOLF_SYSTICK_H_ */
