/* Host shim: map the frodo-go ESP32 heap calls onto plain malloc so the
 * vendored Frodo sources compile unchanged on the PC host harness. */
#pragma once
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

/* ---- DEVICE-FAITHFUL I/O: the device FS allows only ONE open file at a time
 * (gw_littlefs MAX_OPEN_FILES=1). Opening a 2nd file while one is open corrupts the
 * first handle on-device — that is exactly what made c64_diag's /c64_diag.txt clobber
 * the open .d64 so the 1541 looped re-reading the directory and never loaded. The old
 * harness used unbounded stdio and never caught it. Route the vendored Frodo sources'
 * fopen/fclose through h_fopen/h_fclose, which ABORT on a concurrent 2nd open so this
 * whole class of bug fails loudly on the host. (host_glue.cpp #undef's these for its
 * own bookkeeping files — ROM loads, the PPM dump.) */
#ifdef __cplusplus
extern "C" {
#endif
FILE *h_fopen(const char *path, const char *mode);
int   h_fclose(FILE *f);
#ifdef __cplusplus
}
#endif
#define fopen  h_fopen
#define fclose h_fclose
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DMA    0
#define MALLOC_CAP_8BIT   0
#define MALLOC_CAP_INTERNAL 0
#ifdef __cplusplus
extern "C" {
#endif
/* DEVICE-FAITHFUL heap: route heap_caps_malloc + operator new through a bounded
 * ~100KB bump pool that matches the device's C64 overlay heap (badheap). Plain
 * malloc was UNBOUNDED and hid every device-heap OOM (e.g. the 260KB GCR buffer).
 * dev_heap_alloc() aborts + prints the size on overflow = the device's assert. */
void  *dev_heap_alloc(size_t sz);              /* defined in host_glue.cpp */
size_t dev_heap_used(void);
static inline void  *heap_caps_malloc(size_t sz, int caps){ (void)caps; return dev_heap_alloc(sz); }
static inline size_t heap_caps_get_free_size(int caps){ (void)caps; return 0; }
static inline size_t esp_get_free_heap_size(void){ return (size_t)dev_heap_used(); }
#ifdef __cplusplus
}
#endif
