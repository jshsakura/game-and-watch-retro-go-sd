#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DMA    0
#define MALLOC_CAP_8BIT   0
#define MALLOC_CAP_INTERNAL 0
#ifdef __cplusplus
extern "C" {
#endif
void *heap_alloc_mem(size_t s);   /* firmware C++ heap (heap.cpp) */
static inline void  *heap_caps_malloc(size_t sz, int caps){ (void)caps; return heap_alloc_mem(sz); }
static inline size_t heap_caps_get_free_size(int caps){ (void)caps; return 0; }
#ifdef __cplusplus
}
#endif
