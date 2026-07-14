/* The flash cache, on a fake flash that really does get erased.
 *
 * This compiles the REAL Core/Src/gw_flash_alloc.c — the ring every core's ROM
 * goes through — against a byte array that behaves like the chip: erase clears a
 * whole block to 0xFF, program only clears bits. So when the allocator writes
 * over something, the bytes change, and the test can simply ask afterwards
 * whether the ROM is still the ROM.
 *
 * Why it exists
 * -------------
 * Super Metroid: New Game was a black screen, Continue was fine, and the launch
 * after that worked. The port executes no 65816 — it only READS the ROM — so a
 * hole in one region takes out only the scenes that read from there, and the
 * intro's text tables (bank $8B) are read by nothing else.
 *
 * The suspect: the cache is a ring. The launcher caches the ROM and hands the
 * core the address; the core then caches its own blob; and if the ring has come
 * back round, THAT write lands on the ROM the core is already reading out of.
 * The allocator does notice — invalidate_overwritten_files() — but only after
 * the write, when the pointer is long gone.
 *
 * That was a story. Two attempts to fix it before proving it broke the console
 * twice. So it gets proven here, on the host, where being wrong is free.
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

#define FLASH_SIZE   (8u * 1024 * 1024)   /* the ring under test */
#define ROM_SIZE     (3u * 1024 * 1024)   /* Super Metroid */
#define BLOB_SIZE    (70u * 1024)         /* sm.xip */
#define FILLER_SIZE  (1u * 1024 * 1024)   /* other games, cached in between */

static int failures = 0;

static void ok(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "OK  " : "FAIL", what);
    if (!cond) failures++;
}

/* A file whose bytes are a function of its offset, so any hole is obvious. */
static void write_pattern_file(const char *path, uint32_t size, uint8_t seed)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FATAL cannot write %s\n", path); exit(1); }
    uint8_t buf[4096];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
        for (uint32_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)((off + i) * 31u + seed);
        uint32_t want = (size - off < sizeof(buf)) ? size - off : sizeof(buf);
        fwrite(buf, 1, want, f);
    }
    fclose(f);
}

static bool flash_matches_file(const uint8_t *flash, const char *path, uint32_t size,
                              uint32_t *first_bad)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
        uint32_t want = (size - off < sizeof(buf)) ? size - off : sizeof(buf);
        if (fread(buf, 1, want, f) != want) { fclose(f); return false; }
        if (memcmp(buf, flash + off, want) != 0) {
            for (uint32_t i = 0; i < want; i++)
                if (buf[i] != flash[off + i]) { *first_bad = off + i; break; }
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static void fresh_cache(void)
{
    fake_flash_create(FLASH_SIZE);
    remove("saves/flashcachedata.bin");
    mkdir("saves", 0777);
    flash_alloc_reset();
}

/* The allocator hands out 0x9xxxxxxx addresses; the fake flash is a real array.
 * Translate. */
static const uint8_t *flash_bytes(const uint8_t *device_address)
{
    uint32_t off = (uint32_t)(uintptr_t)device_address - FAKE_EXTFLASH_BASE;
    return fake_flash_at(off);
}

int main(void)
{
    printf("=== flash cache: a write must not land on a file being read ===\n");

    write_pattern_file("rom.bin", ROM_SIZE, 0x11);
    write_pattern_file("blob.bin", BLOB_SIZE, 0x22);
    write_pattern_file("filler.bin", FILLER_SIZE, 0x33);

    fresh_cache();

    /* 1. The launcher caches the ROM and hands the core its address. From here
     *    on the core is READING it — this is the address the game plays out of. */
    uint32_t rom_len = 0;
    uint8_t *rom = store_file_in_flash("rom.bin", &rom_len, false, NULL);
    ok(rom != NULL && rom_len == ROM_SIZE, "the ROM caches");

    uint32_t bad = 0;
    ok(flash_matches_file(flash_bytes(rom), "rom.bin", ROM_SIZE, &bad),
       "the ROM in flash matches the card");

    /* 2. Time passes. Other games get played — each in its own boot, because
     *    leaving a game resets the console — and their ROMs go into the ring,
     *    walking the write pointer round until it is about to come back over the
     *    ROM. Five 1 MB fillers in an 8 MB ring do it.
     *
     *    The reboots matter: nothing from a previous game is still being read, so
     *    those ROMs are fair game to overwrite. Only THIS launch's files are not. */
    for (int i = 0; i < 5; i++) {
        flash_alloc_forget_live_files();            /* the console reboots */
        char name[32];
        snprintf(name, sizeof(name), "filler%d.bin", i);
        rename("filler.bin", name);
        write_pattern_file("filler.bin", FILLER_SIZE, (uint8_t)(0x40 + i));
        uint32_t len = 0;
        store_file_in_flash(name, &len, false, NULL);
    }

    /* 3. Now Super Metroid is launched again. The launcher caches the ROM — a
     *    HIT, nothing is written — and hands the core the address. */
    flash_alloc_forget_live_files();                 /* the console reboots */
    uint32_t rom_len2 = 0;
    rom = store_file_in_flash("rom.bin", &rom_len2, false, NULL);
    ok(rom != NULL, "the ROM is still cached (a hit, no write)");

    /* 4. And the core caches its blob — with the ROM's address in hand. */
    uint32_t blob_len = 0;
    uint8_t *blob = store_file_in_flash("blob.bin", &blob_len, false, NULL);
    ok(blob != NULL, "the blob caches");
    if (blob == NULL) { printf("\nFAILED (the blob had nowhere to go)\n"); return 1; }

    /* 5. The whole question. */
    bad = 0;
    bool intact = flash_matches_file(flash_bytes(rom), "rom.bin", ROM_SIZE, &bad);
    if (!intact)
        printf("       the ROM was holed at file offset 0x%06X (flash 0x%08X)\n",
               bad, (unsigned)((uint32_t)(uintptr_t)rom + bad));
    ok(intact, "the ROM the core is reading is still intact after the blob's write");

    /* 6. And the blob has to be the blob — protecting the ROM must not have
     *    silently dropped this write somewhere useless. */
    ok(flash_matches_file(flash_bytes(blob), "blob.bin", BLOB_SIZE, &bad),
       "the blob in flash matches its file");

    fake_flash_destroy();
    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
