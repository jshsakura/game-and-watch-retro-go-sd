/* A cache hit must prove the flash still holds the file.
 *
 * Why it exists
 * -------------
 * The metadata (which file lives where) is saved on the SD card, separately
 * from the flash it describes. Anything that changes the flash behind that
 * record leaves a hit pointing at a hole: a brown-out between an erase and the
 * metadata save, a lost SD write, an external reflash of the same region. The
 * caller then walks away holding a perfectly plausible address into garbage,
 * and for a ROM or a core's XIP code that is a silent wrong-answer or a
 * Hardfault with no message.
 *
 * Each entry now carries the CRC of the bytes as they were programmed (after
 * byte-swap and relocation), and a hit is served only if the flash still
 * matches; otherwise it is a miss and the file is cached again.
 *
 * This compiles the REAL Core/Src/gw_flash_alloc.c on the fake flash.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gw_flash_alloc.h"
#include "gw_linker.h"
#include "flash_stubs.h"

#define FLASH_SIZE (8u * 1024 * 1024)
#define FILE_SIZE  (300u * 1024)

static int failures = 0;
static void ok(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

static void write_pattern_file(const char *path, uint32_t size, uint8_t seed)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FATAL cannot write %s\n", path); exit(1); }
    for (uint32_t i = 0; i < size; i++) fputc((uint8_t)(i * 31u + seed), f);
    fclose(f);
}

static bool flash_matches_file(const uint8_t *device_address, const char *path, uint32_t size)
{
    const uint8_t *flash = fake_flash_at((uint32_t)((uintptr_t)device_address - FAKE_EXTFLASH_BASE));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool same = true;
    for (uint32_t i = 0; i < size && same; i++)
        if ((uint8_t)fgetc(f) != flash[i]) same = false;
    fclose(f);
    return same;
}

int main(void)
{
    printf("=== flash cache: a hit must prove the flash still holds the file ===\n");
    write_pattern_file("crc_file.bin", FILE_SIZE, 0x5a);

    fake_flash_create(FLASH_SIZE);
    remove("saves/flashcachedata.bin");
    mkdir("saves", 0777);
    flash_alloc_reset();

    /* 1. First launch caches the file. */
    uint32_t len = 0;
    uint8_t *first = store_file_in_flash("crc_file.bin", &len, false, NULL);
    ok(first != NULL && len == FILE_SIZE, "the file caches");
    ok(first != NULL && flash_matches_file(first, "crc_file.bin", FILE_SIZE), "flash holds the file");

    /* 2. Next boot, nothing touched the flash: a hit at the same address, no rewrite. */
    flash_alloc_forget_live_files();
    len = 0;
    uint8_t *again = store_file_in_flash("crc_file.bin", &len, false, NULL);
    ok(again == first, "an intact entry is still a hit at the same address");

    /* 3. Something changes the flash behind the metadata's back (a lost erase
     *    record, an external reflash): one byte in the middle of the file. */
    uint8_t *victim = (uint8_t *)fake_flash_at((uint32_t)((uintptr_t)first - FAKE_EXTFLASH_BASE)) + FILE_SIZE / 2;
    *victim ^= 0xff;
    ok(!flash_matches_file(first, "crc_file.bin", FILE_SIZE), "(the flash now differs from the file)");

    /* 4. Next boot: the entry must NOT be served as a hit. The file is cached
     *    again and what comes back is the real thing. */
    flash_alloc_forget_live_files();
    len = 0;
    uint8_t *healed = store_file_in_flash("crc_file.bin", &len, false, NULL);
    ok(healed != NULL, "the damaged entry is re-cached, not refused");
    ok(healed != NULL && flash_matches_file(healed, "crc_file.bin", FILE_SIZE),
       "what the caller gets matches the file (no hole handed out)");

    fake_flash_destroy();
    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
