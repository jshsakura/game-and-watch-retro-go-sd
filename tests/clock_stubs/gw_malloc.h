#ifndef STUB_GW_MALLOC_H
#define STUB_GW_MALLOC_H
#include <stddef.h>
void *ram_malloc(size_t size);
void *ram_calloc(size_t count, size_t size);
size_t ram_get_free_size(void);
size_t ram_mark(void);
void ram_release(size_t mark);
#endif
