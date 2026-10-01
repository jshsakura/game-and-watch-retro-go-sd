/* A relocated XIP code blob must start on a 64 KB boundary.
 *
 * Why it exists
 * -------------
 * The same 32X firmware ran After Burner at two speeds, 14.45 fps and
 * 13.95 fps, and which one you got was decided at boot. Six device runs on
 * 2026-10-01 split exactly by where 32x.xip had landed in external flash:
 * every copy at an 8 KB boundary was the fast one, every copy 4 KB past one
 * was the slow one. The ring aligned files to the 4 KB erase sector only, so
 * the phase depended on whatever had been cached before.
 *
 * 8 KB is one way of the Cortex-M7 I-cache here (16 KB, 2-way, 32 B lines):
 * the address bits that pick a cache set stop at bit 12. Shift the code by
 * 4 KB and every hot function competes for different sets than the ones the
 * link-time layout gave it. That is why a benchmark could not repeat itself
 * within 3%, and why the build a user gets could be the slow one.
 *
 * A later build split again, 18.31 / 18.11 fps, exactly by address mod 32 KB.
 * So the allocator now pins the phase at 64 KB, the large erase block, which
 * covers both.
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

#define FLASH_SIZE   (8u * 1024 * 1024)
#define SECTOR       (4u * 1024)
#define WAY          (64u * 1024)   /* pins the 8 KB and 32 KB phases */
#define BLOB_SIZE    (70u * 1024)

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
    for (uint32_t i = 0; i < size; i++)
        fputc((uint8_t)(i * 31u + seed), f);
    fclose(f);
}

static void no_relocation(uint8_t *buffer, uint32_t size, uint32_t offset,
                          uint8_t *flash_address, uint32_t total)
{
    (void)buffer; (void)size; (void)offset; (void)flash_address; (void)total;
}

static uint32_t phase(const uint8_t *p) { return (uint32_t)(uintptr_t)p % WAY; }

int main(void)
{
    printf("=== flash cache: XIP code lands on a 64 KB boundary ===\n");

    write_pattern_file("one_sector.bin", SECTOR, 0x11);
    write_pattern_file("plain.bin", 2 * SECTOR, 0x33);   /* ends on a way boundary + 4 KB */
    write_pattern_file("blob.xip", BLOB_SIZE, 0x22);

    fake_flash_create(FLASH_SIZE);
    remove("saves/flashcachedata.bin");
    mkdir("saves", 0777);
    flash_alloc_reset();

    /* 1. Something one sector long goes in first, so the write pointer sits
     *    4 KB past a way boundary: the slow phase. */
    uint32_t len = 0;
    uint8_t *data = store_file_in_flash("one_sector.bin", &len, false, NULL);
    ok(data != NULL, "a one-sector file caches");
    ok(phase(data + SECTOR) == SECTOR, "the next free byte is 4 KB past a boundary");

    /* 2. Plain data does not care about the I-cache and keeps packing by sector. */
    flash_alloc_forget_live_files();
    uint32_t plain_len = 0;
    uint8_t *plain = store_file_in_flash("plain.bin", &plain_len, false, NULL);
    ok(plain != NULL && phase(plain) == SECTOR, "an unrelocated file still packs on 4 KB");

    /* 3. The code blob is relocated, so it is code: it skips to the next way
     *    64 KB boundary rather than taking the phase it was handed. */
    flash_alloc_forget_live_files();
    uint32_t blob_len = 0;
    uint8_t *code = store_file_in_flash_relocate("blob.xip", &blob_len, false, NULL, no_relocation);
    ok(code != NULL, "the code blob caches");
    if (code == NULL) { printf("\nFAILED\n"); return 1; }
    printf("       code blob at 0x%08X (phase 0x%04X)\n",
           (unsigned)(uintptr_t)code, (unsigned)phase(code));
    ok(phase(code) == 0, "relocated code starts on a 64 KB boundary");

    /* 4. And the next boot gets that same copy back as a hit. */
    flash_alloc_forget_live_files();
    blob_len = 0;
    uint8_t *again = store_file_in_flash_relocate("blob.xip", &blob_len, false, NULL, no_relocation);
    ok(again == code, "the next boot is a hit at the same aligned address");

    fake_flash_destroy();
    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
