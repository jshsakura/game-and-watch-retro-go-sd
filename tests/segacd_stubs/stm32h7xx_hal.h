#ifndef SEGACD_STUBS_HAL_H
#define SEGACD_STUBS_HAL_H
#include <stdint.h>
/* The probe's halt loop calls HAL_Delay(50) forever. The stub longjmps out on
 * the first call (see stubs.c) so the host test can continue after the probe
 * "halts" without killing the process. */
void HAL_Delay(uint32_t ms);
#endif
