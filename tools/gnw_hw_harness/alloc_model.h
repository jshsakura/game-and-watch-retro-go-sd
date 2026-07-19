#ifndef GNW_HW_ALLOC_MODEL_H
#define GNW_HW_ALLOC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GNW_ALLOC_POISON_BYTE 0xAAu

typedef struct {
    const char *name;
    uint8_t *storage;
    size_t capacity;
    size_t used;
    size_t high_water;
    size_t failed_requests;
} gnw_alloc_pool_t;

/* preused models bytes consumed before a core starts. It is deliberately a
 * runtime input: DTCM launcher use is measured on device, not present in a map. */
bool gnw_alloc_pool_init(gnw_alloc_pool_t *pool, const char *name,
                         void *storage, size_t capacity, size_t preused);
void *gnw_alloc_malloc(gnw_alloc_pool_t *pool, size_t size);
void *gnw_alloc_calloc(gnw_alloc_pool_t *pool, size_t count, size_t size);
size_t gnw_alloc_free_bytes(const gnw_alloc_pool_t *pool);

#endif
