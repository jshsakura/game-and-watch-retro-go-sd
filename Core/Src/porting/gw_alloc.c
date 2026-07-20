#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "gw_linker.h"

static char *heap_end = 0;

/* Bytes handed out of the DTCM stdlib heap so far. Reported around the
 * logo-cache free at emulator_start so a device log says how much the
 * leak was actually worth on this card, instead of only telling us after
 * an allocation has already failed. */
size_t gw_heap_used(void)
{
    if (heap_end == 0)
        return 0;
    return (size_t)(heap_end - (char *)&_heap_start);
}

size_t gw_heap_total(void)
{
    return (size_t)((char *)&_heap_end - (char *)&_heap_start);
}

void *
_sbrk (int incr)
{
    char *        prev_heap_end;

    if (heap_end == 0)
        heap_end = (char *) &_heap_start;

    if ((heap_end + incr) >= (char *)(&_heap_end)) {
        printf("HEAP OOM: need=%d used=%d/%d\n",
               incr, (int)(heap_end - (char *)&_heap_start),
               (int)((char *)&_heap_end - (char *)&_heap_start));
        /* POSIX contract: sbrk reports failure with (void *)-1, which makes
         * malloc() return NULL so the caller's own out-of-memory path runs.
         * This used to assert(0) instead, which turned every soft-handled
         * allocation failure into a fatal exception — picodrive, for one,
         * already degrades cleanly (cart.c: "if (Pico.sv.data == NULL)
         * Pico.sv.flags &= ~SRF_ENABLED"), but never got the chance.
         * The log line above stays, so an OOM is still loud and traceable;
         * it just no longer kills a core that was prepared to cope. */
        return (void *)-1;
    }

    prev_heap_end = heap_end;
    heap_end += incr;

    return (void *) prev_heap_end;
}

#ifdef DEBUG_RG_ALLOC

static struct {
    size_t   total_alloc_bytes;
    size_t   total_alloc_bytes_actual;
    uint32_t total_alloc_num;
} alloc_data;

void *rg_alloc(size_t size, uint32_t caps)
{
    uint32_t *p = malloc(size + sizeof(uint32_t));

    alloc_data.total_alloc_bytes += size;
    alloc_data.total_alloc_bytes_actual += size + sizeof(uint32_t);

#ifdef DEBUG_RG_ALLOC_PRINT
    printf("A %d %d %d %p\n", size, alloc_data.total_alloc_bytes, alloc_data.total_alloc_bytes_actual, p);
#endif

    p[0] = size;

    return &p[1];
}

void *rg_calloc(size_t nmemb, size_t size)
{
    uint8_t *p = rg_alloc(nmemb * size, 0);

    memset(p, '\x00', nmemb * size);

    return p;
}

void rg_free(void *ptr)
{
    assert(ptr != NULL);

    uint32_t *p = ((uint32_t *) ptr) - 1;

    alloc_data.total_alloc_bytes -= p[0];
    alloc_data.total_alloc_bytes_actual -= p[0] + sizeof(uint32_t);

#ifdef DEBUG_RG_ALLOC_PRINT
    printf("F %lu %d %d %p\n",
            p[0],
            alloc_data.total_alloc_bytes,
            alloc_data.total_alloc_bytes_actual,
            p);
#endif

    free(p);
}

void *rg_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return rg_alloc(size, 0);
    }

    uint32_t *p = ((uint32_t *) ptr) - 1;

    alloc_data.total_alloc_bytes -= p[0];
    alloc_data.total_alloc_bytes_actual -= p[0] + sizeof(uint32_t);

    alloc_data.total_alloc_bytes += size;
    alloc_data.total_alloc_bytes_actual += size + sizeof(uint32_t);

#ifdef DEBUG_RG_ALLOC_PRINT
    printf("R %u %d %d %p\n",
            size,
            alloc_data.total_alloc_bytes,
            alloc_data.total_alloc_bytes_actual,
            p);
#endif

    return realloc(p, size);
}

#endif
