#ifndef _MAIN_MD32X_H_
#define _MAIN_MD32X_H_

#include <stdint.h>

/* Launcher entry point for the Sega 32X (picodrive) core. Kept un-namespaced
 * (NOT renamed by md32x_redefines) so rg_emulators can dispatch to it. */
void app_main_md32x(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

/* 32X compositor Draw2FB (328x256 CLUT line buffer), ahb_calloc'd once at
 * load in main_md32x.c's PicoDraw2SetOutBuf. Shared here so md32x_profile.c
 * can _Static_assert that its own AHB request plus this one fits the
 * dynamic AHB pool -- the 0720 device Hardfault ("current_ahb_pointer <=
 * __ahbram_audio_start__" assert in gw_malloc.c:ahb_only_malloc) this pins
 * down for good: ahb_calloc does NOT return NULL on pool overflow, it
 * asserts, so the only real defense is not requesting past the ceiling. */
#define MD32X_D2FB_LINE   328
#define MD32X_D2FB_BYTES  (MD32X_D2FB_LINE * (8 + 240 + 8) + 8)

/* Dynamic AHB pool available to ahb_calloc/ahb_malloc for ANY core, after
 * the GBA's static .gba_ahbram reservation and the 8K non-cacheable .audio
 * DMA slot (STM32H7B0VBTx_SDCARD.ld ._ahbram: __ahbram_heap_start__ ..
 * __ahbram_audio_start__). Linker symbols aren't compile-time constants, so
 * this is hardcoded -- cross-checked by `tools/gnw_hw_harness/run.sh
 * --proposal ahb:<bytes>:<label>` before every commit that touches AHB
 * sizing (the 0720 crash shipped without that check). */
#define MD32X_AHB_DYNAMIC_POOL_BYTES  87904u

#endif /* _MAIN_MD32X_H_ */
