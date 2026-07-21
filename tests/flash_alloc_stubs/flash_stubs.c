/* A fake external flash that behaves like the real one: you must erase before
 * you program, an erase clears whole blocks to 0xFF, and a program only clears
 * bits. So when the allocator writes over something, the bytes really do change
 * — which is the whole point: the test can then ask whether the ROM survived.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "flash_stubs.h"

#define ERASE_BLOCK_SMALL (4u * 1024)
#define ERASE_BLOCK_LARGE (64u * 1024)

static uint8_t *g_flash;
static uint32_t g_flash_size;
static bool     g_memory_mapped = true;

void fake_flash_create(uint32_t size)
{
    free(g_flash);
    g_flash = malloc(size);
    memset(g_flash, 0xFF, size);      /* erased */
    g_flash_size = size;
    g_memory_mapped = true;
}

void fake_flash_destroy(void)
{
    free(g_flash);
    g_flash = NULL;
    g_flash_size = 0;
}

const uint8_t *fake_flash_at(uint32_t offset) { return g_flash + offset; }

/* ---- the OSPI API gw_flash_alloc.c calls ---- */

uint32_t OSPI_GetFlashSize(void)         { return g_flash_size; }
uint32_t OSPI_GetSmallestEraseSize(void) { return ERASE_BLOCK_SMALL; }

void OSPI_EnableMemoryMappedMode(void)  { g_memory_mapped = true; }
void OSPI_DisableMemoryMappedMode(void) { g_memory_mapped = false; }

/* Erases one block at *address and advances it, largest block the alignment
 * allows — exactly what the real driver does, and why a write consumes more
 * than the file's own bytes. */
bool OSPI_Erase(uint32_t *address, uint32_t *size, bool blocking)
{
    (void)blocking;
    uint32_t block = ((*address % ERASE_BLOCK_LARGE) == 0 && *size >= ERASE_BLOCK_LARGE)
                       ? ERASE_BLOCK_LARGE : ERASE_BLOCK_SMALL;

    if (*address + block > g_flash_size) {
        printf("  STUB: erase past the end (%u + %u > %u)\n", *address, block, g_flash_size);
        abort();
    }
    memset(g_flash + *address, 0xFF, block);

    *address += block;
    *size = (*size > block) ? (*size - block) : 0;
    return true;
}

void OSPI_Program(uint32_t address, const uint8_t *buffer, size_t buffer_size)
{
    if (g_memory_mapped) {
        printf("  STUB: programmed while still memory-mapped\n");
        abort();
    }
    if (address + buffer_size > g_flash_size) {
        printf("  STUB: program past the end\n");
        abort();
    }
    /* Programming only clears bits — writing into unerased flash corrupts, it
     * does not overwrite. Model it, so a missing erase cannot pass unnoticed. */
    for (size_t i = 0; i < buffer_size; i++)
        g_flash[address + i] &= buffer[i];
}

/* ---- the rest of the device the allocator touches ---- */

uint32_t HAL_GetUIDw0(void) { return 0xAAAA0001; }
uint32_t HAL_GetUIDw1(void) { return 0xBBBB0002; }
uint32_t HAL_GetUIDw2(void) { return 0xCCCC0003; }

void wdog_refresh(void) {}

uint32_t get_ofw_extflash_size(void) { return 0; }

/* zlib's polynomial, same as the firmware's crc32_le() — only used as a key. */
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}
