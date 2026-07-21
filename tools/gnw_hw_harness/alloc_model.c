#include "alloc_model.h"

#include <limits.h>
#include <string.h>

static bool align4(size_t value, size_t *aligned)
{
    if (value > SIZE_MAX - 3u) return false;
    *aligned = (value + 3u) & ~(size_t)3u;
    return true;
}

bool gnw_alloc_pool_init(gnw_alloc_pool_t *pool, const char *name,
                         void *storage, size_t capacity, size_t preused)
{
    if (!pool || !storage || preused > capacity) return false;
    pool->name = name;
    pool->storage = storage;
    pool->capacity = capacity;
    pool->used = preused;
    pool->high_water = preused;
    pool->failed_requests = 0;
    /* Never grant the zero-filled-host-heap accident. calloc explicitly
     * clears its own return range below; malloc always exposes poison. */
    memset(storage, GNW_ALLOC_POISON_BYTE, capacity);
    return true;
}

void *gnw_alloc_malloc(gnw_alloc_pool_t *pool, size_t size)
{
    size_t aligned;
    if (!pool || !align4(size, &aligned) || aligned > pool->capacity - pool->used) {
        if (pool) pool->failed_requests++;
        return NULL;
    }
    void *result = pool->storage + pool->used;
    pool->used += aligned;
    if (pool->used > pool->high_water) pool->high_water = pool->used;
    memset(result, GNW_ALLOC_POISON_BYTE, size);
    return result;
}

void *gnw_alloc_calloc(gnw_alloc_pool_t *pool, size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        if (pool) pool->failed_requests++;
        return NULL;
    }
    size_t bytes = count * size;
    void *result = gnw_alloc_malloc(pool, bytes);
    if (result) memset(result, 0, bytes);
    return result;
}

size_t gnw_alloc_free_bytes(const gnw_alloc_pool_t *pool)
{
    return pool ? pool->capacity - pool->used : 0;
}
