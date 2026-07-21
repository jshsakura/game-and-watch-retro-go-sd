/* Host-test stub for Core/Inc/main.h. __NOP()/__WFI() are CMSIS busy-wait
 * primitives; on the device they just burn a cycle while common_emu_sound_sync()
 * waits for dma_counter (gw_audio.h) to change. Here they advance dma_counter
 * directly -- one call == "one DMA half completed" -- so the loop terminates
 * deterministically on a host with no DMA. This is the only seam in this file. */
#ifndef STUB_MAIN_H
#define STUB_MAIN_H
#include <stdint.h>

/* `uint` (bare, no _t) is a glibc/BSD extension common.c relies on via the
 * device toolchain's headers; give it explicitly rather than pull in
 * sys/types.h and whatever else comes with it. */
typedef unsigned int uint;

#define __NOP() (dma_counter++)
#define __WFI() (dma_counter++)

void SystemClock_Config(uint8_t new_oc_level);

#endif
