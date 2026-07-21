/* Host stand-in for the firmware allocators the sm core calls under TARGET_GNW.
 * Only the declarations matter: what we are checking is whether the device's
 * source set resolves every symbol it needs. */
#pragma once
#include <stddef.h>
void *itc_calloc(size_t count, size_t size);
void *itc_malloc(size_t size);
void *ahb_malloc(size_t size);
void *ram_malloc(size_t size);
void *ram_calloc(size_t count, size_t size);
