/*
 * Build-test for Phase 11 (docs/CPS1_MAME_ALIGNMENT.md sections 7/9): the
 * wof-specific GFX ROM bank mapper, byte-interleaved multi-chip loading
 * (cps1_rom_load_interleaved, built in Phase 4 but never actually
 * exercised by a test until now), and the 68000 reset-vector sanity check
 * a real ROM load must pass before anything tries to run it. No real
 * CPS-1 ROM exists in this repo -- every case here fabricates its own
 * synthetic chip files (hand-computed expected interleave, not compared
 * against the implementation's own formula).
 *
 *   ./build/cps1-rom-load-selftest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cps1_rom.h"
#include "cps1_rom_linux.h"

static int failures = 0;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                          \
            failures++;                                                     \
        }                                                                    \
    } while (0)

/*
 * wof's shift table: SPRITES=1, SCROLL1=0, SCROLL2=1, SCROLL3=3
 * (docs/CPS1_MAME_ALIGNMENT.md section 7). wof's own bank table is 2
 * CONTIGUOUS 32KB banks spanning the whole shifted-code range, so the
 * bank lookup/recompose is a mathematical identity for every in-range
 * shifted value -- this test proves that identity holds (i.e. the bank
 * math genuinely runs and lands back on the input), not that the
 * function skips straight to a `code << shift` shortcut internally.
 */
static void test_bank_mapper_shift(void)
{
    CHECK(cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL1, 100) == 100,
          "SCROLL1 shift=0: expected identity, got %u",
          cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL1, 100));
    CHECK(cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SPRITES, 100) == 100,
          "SPRITES shift=1: expected round-trip identity, got %u",
          cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SPRITES, 100));
    CHECK(cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL2, 100) == 100,
          "SCROLL2 shift=1: expected round-trip identity, got %u",
          cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL2, 100));
    CHECK(cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL3, 100) == 100,
          "SCROLL3 shift=3: expected round-trip identity, got %u",
          cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL3, 100));
}

/* Bank-boundary crossing: SCROLL3 (shift=3) code 0x0FFF shifts to exactly
 * 0x7FF8 (bank 0, the last few slots before the 0x8000 boundary); code
 * 0x1000 shifts to exactly 0x8000 (the FIRST slot of bank 1). Both must
 * still round-trip to their own input -- proving the bank-table lookup
 * itself picks the right entry at the boundary, not just that identity
 * happens to hold somewhere in the middle of one bank. */
static void test_bank_mapper_boundary(void)
{
    uint32_t below = cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL3, 0x0FFFu);
    uint32_t at = cps1_gfxrom_bank_mapper_wof(CPS1_GFXTYPE_SCROLL3, 0x1000u);
    CHECK(below == 0x0FFFu, "SCROLL3 code just below bank1: expected 0x0FFF, got 0x%04x", below);
    CHECK(at == 0x1000u, "SCROLL3 code exactly at bank1 start: expected 0x1000, got 0x%04x", at);
}

static void write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s for write\n", path);
        exit(1);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

/* chip0 = {0x11,0x22,0x33}, chip1 = {0xAA,0xBB,0xCC} -> interleaved
 * {0x11,0xAA,0x22,0xBB,0x33,0xCC} (docs/CPS1_MAME_ALIGNMENT.md section 7 /
 * cps1_rom_linux.h's own doc comment: chip0=1234,chip1=ABCD ->
 * combined=1A2B3C4D). Hand-derived expected output, not re-derived from
 * the implementation's own loop. */
static void test_interleave(void)
{
    char path0[64], path1[64];
    snprintf(path0, sizeof(path0), "/tmp/cps1_selftest_chip0_%d.bin", (int)getpid());
    snprintf(path1, sizeof(path1), "/tmp/cps1_selftest_chip1_%d.bin", (int)getpid());

    uint8_t chip0[3] = { 0x11, 0x22, 0x33 };
    uint8_t chip1[3] = { 0xAA, 0xBB, 0xCC };
    write_file(path0, chip0, sizeof(chip0));
    write_file(path1, chip1, sizeof(chip1));

    const char *paths[2] = { path0, path1 };
    cps1_rom_region_t out = {0};
    int rc = cps1_rom_load_interleaved(&out, paths, 2);

    CHECK(rc == 0, "interleave load should succeed");
    if (rc == 0) {
        static const uint8_t expected[6] = { 0x11, 0xAA, 0x22, 0xBB, 0x33, 0xCC };
        CHECK(out.size == 6, "interleaved size should be 6, got %u", out.size);
        for (int i = 0; i < 6 && out.size == 6; i++)
            CHECK(out.data[i] == expected[i], "byte %d should be 0x%02x, got 0x%02x",
                  i, expected[i], out.data[i]);
        free((void *)out.data);
    }

    remove(path0);
    remove(path1);
}

/* A real PRG ROM's reset vector: SSP=0x00100000 (arbitrary, unchecked),
 * PC=0x00000010 -- INSIDE a 32-byte fabricated ROM, so
 * cps1_rom_check_reset_vector must accept it and report both fields back
 * correctly. */
static void test_reset_vector_valid(void)
{
    uint8_t prg[32];
    memset(prg, 0, sizeof(prg));
    prg[0] = 0x00; prg[1] = 0x10; prg[2] = 0x00; prg[3] = 0x00; /* SSP = 0x00100000 */
    prg[4] = 0x00; prg[5] = 0x00; prg[6] = 0x00; prg[7] = 0x10; /* PC  = 0x00000010 */

    cps1_rom_region_t region = { prg, sizeof(prg) };
    uint32_t ssp = 0, pc = 0;
    int rc = cps1_rom_check_reset_vector(&region, &ssp, &pc);

    CHECK(rc == 0, "valid in-range reset vector should pass");
    CHECK(ssp == 0x00100000u, "SSP should be 0x00100000, got 0x%08x", ssp);
    CHECK(pc == 0x00000010u, "PC should be 0x00000010, got 0x%08x", pc);
}

/* Same ROM, but PC = 0xDEAD0000 -- nowhere near the 32-byte fabricated
 * ROM's own range. Must be rejected, not silently accepted (this is the
 * actual correctness gate: a byte-interleave direction bug or the wrong
 * file produces exactly this kind of wildly-out-of-range PC). */
static void test_reset_vector_invalid(void)
{
    uint8_t prg[32];
    memset(prg, 0, sizeof(prg));
    prg[4] = 0xDE; prg[5] = 0xAD; prg[6] = 0x00; prg[7] = 0x00; /* PC = 0xDEAD0000 */

    cps1_rom_region_t region = { prg, sizeof(prg) };
    int rc = cps1_rom_check_reset_vector(&region, NULL, NULL);
    CHECK(rc != 0, "out-of-range PC (0xDEAD0000 vs 32-byte ROM) must be rejected");

    cps1_rom_region_t too_small = { prg, 4 };
    rc = cps1_rom_check_reset_vector(&too_small, NULL, NULL);
    CHECK(rc != 0, "a region too small to even hold a reset vector must be rejected");
}

/* Ties Phase 11's two pieces together: byte-interleave TWO PRG chips
 * (odd/even bytes of a real reset vector, exactly the convention a real
 * CPS-1 PRG ROM dump uses) and confirm the RESULT of that load passes the
 * reset-vector gate -- not just that each piece works in isolation. */
static void test_interleaved_load_then_reset_vector(void)
{
    char path0[64], path1[64];
    snprintf(path0, sizeof(path0), "/tmp/cps1_selftest_prg0_%d.bin", (int)getpid());
    snprintf(path1, sizeof(path1), "/tmp/cps1_selftest_prg1_%d.bin", (int)getpid());

    /* Target reset vector after interleave: SSP=0x00100000, PC=0x00000100
     * (INSIDE the 512-byte combined ROM below -- 0x100 < 0x200). Interleaved
     * big-endian bytes {SSP,PC} = 00 10 00 00 00 00 01 00 -> even-index
     * chip (chip0) gets bytes [0,2,4,6] = 00,00,00,01 ; odd-index chip
     * (chip1) gets [1,3,5,7] = 10,00,00,00. */
    uint8_t chip0[256], chip1[256];
    memset(chip0, 0, sizeof(chip0));
    memset(chip1, 0, sizeof(chip1));
    chip0[0] = 0x00; chip1[0] = 0x10; /* byte0=SSP hi,  byte1 */
    chip0[1] = 0x00; chip1[1] = 0x00; /* byte2,         byte3 */
    chip0[2] = 0x00; chip1[2] = 0x00; /* byte4=PC hi,   byte5 */
    chip0[3] = 0x01; chip1[3] = 0x00; /* byte6,         byte7=PC lo */

    write_file(path0, chip0, sizeof(chip0));
    write_file(path1, chip1, sizeof(chip1));

    const char *paths[2] = { path0, path1 };
    cps1_rom_region_t prg = {0};
    int rc = cps1_rom_load_interleaved(&prg, paths, 2);
    CHECK(rc == 0, "PRG interleave load should succeed");

    if (rc == 0) {
        CHECK(prg.size == 512, "interleaved PRG size should be 512, got %u", prg.size);
        uint32_t ssp = 0, pc = 0;
        int vec_rc = cps1_rom_check_reset_vector(&prg, &ssp, &pc);
        CHECK(vec_rc == 0, "interleaved PRG's reset vector should pass the range gate");
        CHECK(ssp == 0x00100000u, "interleaved SSP should be 0x00100000, got 0x%08x", ssp);
        CHECK(pc == 0x00000100u, "interleaved PC should be 0x00000100, got 0x%08x", pc);
        free((void *)prg.data);
    }

    remove(path0);
    remove(path1);
}

int main(void)
{
    test_bank_mapper_shift();
    test_bank_mapper_boundary();
    test_interleave();
    test_reset_vector_valid();
    test_reset_vector_invalid();
    test_interleaved_load_then_reset_vector();

    if (failures) {
        fprintf(stderr, "[cps1-rom-load-selftest] FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-rom-load-selftest] OK\n");
    return 0;
}
