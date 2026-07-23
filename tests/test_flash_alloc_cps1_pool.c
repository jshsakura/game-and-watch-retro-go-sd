/* The live-file protection ring (gw_flash_alloc.c) sized itself against "a MAME
 * romset... eleven addresses all held at once" (see its own comment, chosen when
 * CPS-1 shipped its FIRST folder-loader phase: two program chips + eight graphics
 * chips + the core's own XIP code blob = 11). MAX_LIVE_FILES was set to 16 on
 * that basis, called "a capacity, not a per-core special case".
 *
 * That arithmetic stopped covering CPS-1 the moment subfolder pooling shipped
 * (commit afa5b8c6): a game folder holding BOTH a parent set and its clone in
 * separate subfolders (exactly wof/ + wofj/, this project's own test ROM pair,
 * and exactly the scenario the pooling feature exists to support) pools their
 * chips into ONE list. tools/cps1_romsets.json says the union of wofj's 10
 * chips and wofr1's 10 chips is 16 DISTINCT chips (4 shared, not duplicated on
 * disk). Add the core's own XIP blob and a single boot caches 17 files in a row
 * with no reboot in between -- one more than MAX_LIVE_FILES protects.
 *
 * The 17th call to store_file_in_flash() (whichever chip is scanned last) still
 * gets a valid, non-overlapping address -- find_write_slot() checks it against
 * the 16 already-tracked ranges same as any other write. What it does NOT get is
 * a live_files[] entry of its own, because live_add() silently drops anything
 * past MAX_LIVE_FILES. For the rest of THIS boot, that file's flash range reads
 * as "nobody is using this" to every write that comes after it -- and unlike the
 * two-file scenario tests/test_flash_alloc.c proves safe, a wraparound write in
 * THIS shape lands precisely on it, not on protected space, because the walk in
 * find_write_slot() skips only what live_overlaps() tracks.
 *
 * RED first, and against the real thing: the same test runs over the allocator
 * at MAX_LIVE_FILES=16 (git show), where the 18th write corrupts the 17th file.
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

#define CHIP_SIZE   (512u * 1024)   /* CPS1_ROMSET_CHIP_SIZE */
#define N_FILES     17              /* xip + wof's 10 + wofj's 6 -- real union */
#define FLASH_SIZE  (CHIP_SIZE * (N_FILES))   /* zero slack: the 18th MUST wrap */

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
    uint8_t buf[4096];
    for (uint32_t off = 0; off < size; off += sizeof(buf)) {
        for (uint32_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)((off + i) * 31u + seed);
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
}

static bool flash_matches_file(const uint8_t *flash, const char *path, uint32_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    bool ok_ = true;
    for (uint32_t off = 0; off < size && ok_; off += sizeof(buf)) {
        if (fread(buf, 1, sizeof(buf), f) != sizeof(buf)) { ok_ = false; break; }
        if (memcmp(buf, flash + off, sizeof(buf)) != 0)
            ok_ = false;
    }
    fclose(f);
    return ok_;
}

static const uint8_t *flash_bytes(const uint8_t *device_address)
{
    uint32_t off = (uint32_t)(uintptr_t)device_address - FAKE_EXTFLASH_BASE;
    return fake_flash_at(off);
}

int main(void)
{
    printf("=== flash cache: CPS-1's real wof+wofj pool (17 live files) must not "
           "corrupt each other within one boot ===\n");

    fake_flash_create(FLASH_SIZE);
    remove("saves/flashcachedata.bin");
    mkdir("saves", 0777);
    flash_alloc_reset();

    char names[N_FILES][32];
    uint8_t *addr[N_FILES];

    /* Cache exactly the sequence app_main_cps1() does: the XIP blob first, then
     * every chip the folder scan finds, all in the SAME boot (no
     * flash_alloc_forget_live_files() call -- that only happens on reboot). */
    for (int i = 0; i < N_FILES; i++) {
        snprintf(names[i], sizeof(names[i]), "file%02d.bin", i);
        write_pattern_file(names[i], CHIP_SIZE, (uint8_t)(0x10 + i));
        uint32_t len = 0;
        addr[i] = store_file_in_flash(names[i], &len, false, NULL);
        if (addr[i] == NULL || len != CHIP_SIZE) {
            printf("  FATAL file %d did not cache\n", i);
            return 1;
        }
    }

    /* One more write in the SAME boot -- the ring is exactly full, so this MUST
     * wrap. It stands in for anything caching one more file post-load; CPS-1
     * does not today, but the allocator does not know that, and a safety margin
     * that depends on a caller never doing one more thing is not a margin. */
    write_pattern_file("extra.bin", CHIP_SIZE, 0xEE);
    uint32_t extra_len = 0;
    uint8_t *extra = store_file_in_flash("extra.bin", &extra_len, false, NULL);

    /* The whole question: did every one of the 17 files this boot is using
     * survive that one extra write? */
    int corrupted = -1;
    for (int i = 0; i < N_FILES; i++) {
        if (!flash_matches_file(flash_bytes(addr[i]), names[i], CHIP_SIZE)) {
            corrupted = i;
            break;
        }
    }
    if (corrupted >= 0)
        printf("       file %d (%s) was overwritten by the 18th write\n",
               corrupted, names[corrupted]);
    ok(corrupted < 0, "all 17 files this boot needs are still intact after one more write");

    if (extra != NULL)
        ok(flash_matches_file(flash_bytes(extra), "extra.bin", CHIP_SIZE),
           "if the 18th write succeeded, it is at least the file it claims to be");
    else
        printf("  OK  the 18th write found no safe room and failed cleanly (no corruption)\n");

    fake_flash_destroy();
    printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
    return failures ? 1 : 0;
}
