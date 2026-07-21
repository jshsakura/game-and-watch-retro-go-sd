/*
 * Proves the Musashi-backed CPU (cps1_m68k.c) executes REAL 68000 code
 * through the REAL CPS-1 memory map.
 *
 * Every construct below was chosen because the nine-opcode skeleton it
 * replaced (cps1_cpu68k.c) could not execute it AT ALL:
 *
 *   - the reset vector      (skeleton had no exception vectors; PC started at 0)
 *   - LEA                   (absent)
 *   - MOVE.L                (absent -- MOVE.W only)
 *   - JSR + RTS with a real stack (BSR did not push; RTS just set halted=1)
 *   - CMPI.W + Bcc          (no compare instruction existed)
 *   - MOVE.W Dn,(xxx).L     (no absolute-long addressing mode)
 *   - a base-mapped WRAM write and an I/O-callback register write in the
 *     same program (the skeleton fetched from a flat array, not a bus)
 *
 * So this is a RED-then-GREEN test in the sense CLAUDE.md asks for: point
 * it at the old core and it cannot even be expressed, let alone pass.
 *
 * ENDIANNESS: the program is written below as uint16_t words in 68000
 * (big-endian) ORDER, then stored with native 16-bit writes. On a
 * little-endian host that lands the bytes already 16-bit-swapped, which is
 * exactly the layout Musashi's base-pointer path requires (cps1_m68k.h's
 * ENDIANNESS note). Same trick, same reason, as Sega Genesis's
 * GAME_DATA_BYTESWAP_16.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cps1_m68k.h"

#define ROM_BYTES  0x10000u  /* one 64 KB page -- the map is page-granular */
#define WRAM_BYTES 0x10000u

#define STACK_TOP  0x00FF1000u
#define ENTRY_PC   0x00000008u
#define GFXRAM_TARGET 0x900000u

/*
 * 0x000000  .long $00FF1000     initial SSP  (reset vector)
 * 0x000004  .long $00000008     initial PC   (reset vector)
 * 0x000008  LEA   $00FF0000,A0
 * 0x00000E  MOVE.L #$12345678,D0
 * 0x000014  MOVE.L D0,(A0)
 * 0x000016  JSR   $00000040
 * 0x00001C  CMPI.W #$0042,D1
 * 0x000020  BNE.S  .fail
 * 0x000022  MOVEQ #1,D0
 * 0x000024  BRA.S  .store
 * 0x000026  MOVEQ #-1,D0        .fail
 * 0x000028  MOVE.W D0,$00900000 .store
 * 0x00002E  BRA.S  *            (park here; run a bounded cycle budget)
 * 0x000040  MOVE.W #$0042,D1    subroutine
 * 0x000044  RTS
 */
static const uint16_t k_reset_and_code[] = {
    /* 0x0000 */ 0x00FF, 0x1000,          /* SSP */
    /* 0x0004 */ 0x0000, 0x0008,          /* PC  */
    /* 0x0008 */ 0x41F9, 0x00FF, 0x0000,  /* LEA   $00FF0000,A0     */
    /* 0x000E */ 0x203C, 0x1234, 0x5678,  /* MOVE.L #$12345678,D0   */
    /* 0x0014 */ 0x2080,                  /* MOVE.L D0,(A0)         */
    /* 0x0016 */ 0x4EB9, 0x0000, 0x0040,  /* JSR   $00000040        */
    /* 0x001C */ 0x0C41, 0x0042,          /* CMPI.W #$0042,D1       */
    /* 0x0020 */ 0x6604,                  /* BNE.S  -> 0x0026       */
    /* 0x0022 */ 0x7001,                  /* MOVEQ #1,D0            */
    /* 0x0024 */ 0x6002,                  /* BRA.S  -> 0x0028       */
    /* 0x0026 */ 0x70FF,                  /* MOVEQ #-1,D0           */
    /* 0x0028 */ 0x33C0, 0x0090, 0x0000,  /* MOVE.W D0,$00900000    */
    /* 0x002E */ 0x60FE,                  /* BRA.S  * (self)        */
};
static const uint16_t k_subroutine[] = {
    /* 0x0040 */ 0x323C, 0x0042,          /* MOVE.W #$0042,D1       */
    /* 0x0044 */ 0x4E75,                  /* RTS                    */
};

/* --- I/O bridge: records the register/gfxram traffic the program makes --- */
static uint32_t s_io_last_write_addr;
static uint16_t s_io_last_write_val;
static unsigned s_io_write_count;

static uint16_t test_io_read16(uint32_t addr)
{
    (void)addr;
    return 0;
}

static void test_io_write16(uint32_t addr, uint16_t val)
{
    s_io_last_write_addr = addr;
    s_io_last_write_val  = val;
    s_io_write_count++;
}

static int fail(const char *what, unsigned long got, unsigned long want)
{
    printf("[cps1-m68k-selftest] FAIL %s: got 0x%08lx want 0x%08lx\n", what, got, want);
    return 1;
}

int main(void)
{
    uint8_t *rom  = calloc(1, ROM_BYTES);
    uint8_t *wram = calloc(1, WRAM_BYTES);
    if (rom == NULL || wram == NULL) {
        printf("[cps1-m68k-selftest] FAIL: out of memory\n");
        return 1;
    }

    /* Native 16-bit stores => little-endian host lands the byte-swapped
     * layout the base-pointer path needs. See the file header. */
    uint16_t *rom16 = (uint16_t *)(void *)rom;
    for (size_t i = 0; i < sizeof(k_reset_and_code) / sizeof(k_reset_and_code[0]); i++)
        rom16[i] = k_reset_and_code[i];
    for (size_t i = 0; i < sizeof(k_subroutine) / sizeof(k_subroutine[0]); i++)
        rom16[(0x40u / 2u) + i] = k_subroutine[i];

    const cps1_m68k_io_t io = { test_io_read16, test_io_write16 };
    cps1_m68k_init(rom, ROM_BYTES, wram, &io);
    cps1_m68k_reset();

    /* --- the reset vector actually took --- */
    if (cps1_m68k_get_pc() != ENTRY_PC)
        return fail("reset PC", cps1_m68k_get_pc(), ENTRY_PC);
    if (cps1_m68k_get_areg(7) != STACK_TOP)
        return fail("reset SSP (A7)", cps1_m68k_get_areg(7), STACK_TOP);
    printf("[cps1-m68k-selftest] reset vector: PC=0x%08x SSP=0x%08x OK\n",
           cps1_m68k_get_pc(), cps1_m68k_get_areg(7));

    /* Generous budget: the whole program is a couple dozen instructions,
     * then it parks in the self-branch burning cycles harmlessly. */
    cps1_m68k_run(2000);

    /* --- JSR pushed a return address and RTS came back with D1 set --- */
    if (cps1_m68k_get_dreg(1) != 0x42u)
        return fail("D1 after JSR/RTS", cps1_m68k_get_dreg(1), 0x42u);

    /* --- CMPI.W + BNE took the equal path --- */
    if (cps1_m68k_get_dreg(0) != 1u)
        return fail("D0 after CMPI/BNE", cps1_m68k_get_dreg(0), 1u);

    /* --- MOVE.L through a base-mapped page: stored byte-swapped too --- */
    const uint16_t *wram16 = (const uint16_t *)(const void *)wram;
    if (wram16[0] != 0x1234u || wram16[1] != 0x5678u)
        return fail("WRAM MOVE.L", ((unsigned long)wram16[0] << 16) | wram16[1], 0x12345678ul);

    /* --- MOVE.W to absolute long landed on the I/O callback, not memory --- */
    if (s_io_write_count == 0)
        return fail("io write count", s_io_write_count, 1);
    if (s_io_last_write_addr != GFXRAM_TARGET)
        return fail("io write addr", s_io_last_write_addr, GFXRAM_TARGET);
    if (s_io_last_write_val != 1u)
        return fail("io write val", s_io_last_write_val, 1u);

    printf("[cps1-m68k-selftest] LEA/MOVE.L/JSR/RTS/CMPI/Bcc: OK "
           "(D0=%u D1=0x%02x)\n", cps1_m68k_get_dreg(0), cps1_m68k_get_dreg(1));
    printf("[cps1-m68k-selftest] bus: WRAM long=0x%04x%04x, io write 0x%06x=0x%04x (%u writes) OK\n",
           wram16[0], wram16[1], s_io_last_write_addr, s_io_last_write_val, s_io_write_count);
    printf("[cps1-m68k-selftest] STATEHASH=%08x\n", cps1_m68k_state_hash());
    printf("[cps1-m68k-selftest] OK\n");

    free(rom);
    free(wram);
    return 0;
}
