/* Test-only IRQ/intrinsic adapter. ARM uses the actual CMSIS instructions. */
#pragma once
#include <stdint.h>
#ifdef __arm__
#include "cmsis_gcc.h"
#else
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __disable_irq(void) {}
static inline void __set_PRIMASK(uint32_t x) { (void)x; }
static inline uint64_t __SMLALD(uint32_t a, uint32_t b, uint64_t acc) {
  return acc + (uint64_t)((int64_t)(int16_t)a*(int16_t)b)
             + (uint64_t)((int64_t)(int16_t)(a>>16)*(int16_t)(b>>16));
}
#endif
