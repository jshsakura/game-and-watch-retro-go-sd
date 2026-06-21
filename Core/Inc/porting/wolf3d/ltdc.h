#ifndef _WOLF_LTDC_H_
#define _WOLF_LTDC_H_

/* G&W port shim for the Wolf3D-STM32 driver header (src/drivers/ltdc.h).
 * The LTDC hardware-CLUT render path is replaced by a software 8bit->RGB565
 * flush (see wolf_vh.c / gw_wolf_present in main_wolf3d.c). These prototypes
 * exist only to satisfy wl_def.h's #include; they are not called in this build. */
#include <stdint.h>

void ltdc_setup(void);
void ltdc_draw_intro(void);
void ltdc_wait_for_vsync(void);
void ltdc_set_clut(uint8_t *palette);

#endif /* _WOLF_LTDC_H_ */
