#pragma once
/*
 * CPS-1 68000 CPU: Musashi, wired to the CPS-1 memory map.
 *
 * WHY THIS REPLACED cps1_cpu68k.c
 * -------------------------------
 * The Phase 3 skeleton (cps1_cpu68k.c) implemented NINE opcode forms --
 * NOP, RTS, MOVEQ, ADDQ/SUBQ (Dn-direct only), DBcc, Bcc/BRA/BSR (8-bit
 * displacement, BSR not even pushing), MOVEA.L #imm32, and three MOVE.W
 * forms -- with no stack (A7 was never used), no JSR/JMP/LEA/MOVE.L/CMP,
 * no exception vectors, no interrupts, and a fetch16() that read a FLAT
 * cpu->code[] ARRAY rather than the bus. It could run its own synthetic
 * test program and nothing else; a real Tenchi wo Kurau II ROM dies on
 * roughly its first instruction. It was never going to become a 68000.
 *
 * Musashi is the 68000 core MAME uses. This project ALREADY SHIPS IT:
 * external/gwenesis/src/cpus/M68K/m68kcpu.c is the Genesis-Plus-GX flavour
 * of Musashi, and it is what runs Sega Genesis AND Sega CD on this exact
 * STM32H7B0 today (build/md/m68kcpu.o, build/segacd/m68kcpu.o). Adopting
 * it is the repo's own "research and reuse before writing new code" rule
 * applied to the one component where writing new code was never viable.
 *
 * WHAT IT COSTS (measured, arm-none-eabi, -O2, from build/md/m68kcpu.o)
 * --------------------------------------------------------------------
 *   .text total                        493,971 B   (482 KB)
 *     of which m68ki_instruction_jump_table  262,144 B  (rodata)
 *     of which m68ki_cycles                   65,536 B  (rodata)
 *     of which m68ki_exception_cycle_table       512 B  (rodata)
 *     => real code                      ~165,779 B  (162 KB)
 *   .bss                                  5,500 B   (the `m68k` struct)
 *
 * The 320 KB of rodata tables are XIP candidates (external flash, the
 * sm.xip precedent in the linker script) -- they are constant lookup
 * tables, never written. Only ~162 KB of code plus 5.5 KB of state has to
 * be RAM-resident, which is what makes this fit RAM_EMU's 724 KB next to
 * cps1's own ~380 KB of gfxram/WRAM/tilemap/tile-cache.
 *
 * ENDIANNESS -- THE ONE THING THAT WILL SILENTLY CORRUPT EVERYTHING
 * ----------------------------------------------------------------
 * This Musashi reads base-mapped memory as `*(uint16*)(base + (addr &
 * 0xffff))` and bytes as `base[(addr & 0xffff) ^ 1]` (macros.h's
 * LSB_FIRST branch). On a little-endian host/device that means every
 * base-mapped region MUST BE STORED BYTE-SWAPPED (16-bit word swap) --
 * exactly what Sega Genesis already does, which is why rg_emulators.c
 * registers "Sega Genesis" with GAME_DATA_BYTESWAP_16. CPS-1's program
 * ROM needs the same treatment; cps1_m68k_init() does NOT swap for you,
 * because on device the ROM is XIP'd straight out of flash and the swap
 * has to happen at flash-write time (via the launcher's own
 * GAME_DATA_BYTESWAP_16 path), not per access.
 *
 * I/O regions (gfxram, CPS-A/CPS-B) go through the read16/write16
 * callbacks instead, which take a plain 68000-order address and value --
 * no swapping involved, so cps1_core.c's existing bus semantics (palette
 * conversion on gfxram writes, register decode) are reused unchanged.
 */
#include <stdint.h>

/* Word-granular I/O bridge for the regions that have side effects. Address
 * is a real 68000 address (already masked to 24 bits); value is in normal
 * 68000 byte order, NOT byte-swapped. 8-bit accesses are synthesized from
 * these by cps1_m68k.c (read-modify-write for write8), because every CPS-1
 * register and gfxram structure is word-oriented. */
typedef struct {
    uint16_t (*read16)(uint32_t addr);
    void     (*write16)(uint32_t addr, uint16_t val);
} cps1_m68k_io_t;

/*
 * Builds the CPS-1 68000 memory map onto Musashi's 256-entry page table
 * (one entry per 64 KB of the 24-bit address space):
 *
 *   0x000000-0x3FFFFF  program ROM   -> base pointer (XIP-able, 0 RAM cost)
 *   0x800000-0x80FFFF  CPS-A/CPS-B   -> io callbacks
 *   0x900000-0x92FFFF  gfxram (192K) -> io callbacks (writes have side effects)
 *   0xFF0000-0xFFFFFF  work RAM 64K  -> base pointer
 *   everything else                  -> open bus (reads 0xFFFF, writes dropped)
 *
 * `prg` must be byte-swapped (see the ENDIANNESS note above) and stays
 * owned by the caller -- nothing here copies it. prg_size is clamped down
 * to whole 64 KB pages and to the 0x400000 the map reserves for it.
 * `wram` must point at 64 KB the caller owns; it is NOT cleared here.
 */
void cps1_m68k_init(const uint8_t *prg, uint32_t prg_size, uint8_t *wram,
                     const cps1_m68k_io_t *io);

/*
 * Maps one program-ROM CHIP at `dest_offset` in the 68000 address space,
 * after a cps1_m68k_init() that passed prg = NULL.
 *
 * A CPS-1 romset's program ROM is two separate 512 KB chips, and on the
 * device each is cached into external flash as its own file -- so the two
 * halves are at unrelated addresses and there is no 1 MB contiguous buffer to
 * point a single base pointer at. There does not need to be: Musashi's map is
 * one base pointer PER 64 KB PAGE, so each chip is simply mapped over its own
 * eight pages. Both `dest_offset` and `size` must be whole 64 KB pages, and
 * the whole span must fall inside the 0x000000-0x3FFFFF the map reserves for
 * program ROM; anything else is ignored rather than half-applied.
 *
 * `data` must be byte-swapped exactly as cps1_m68k_init()'s `prg` must be --
 * which for a real MAME CPS-1 chip dump means stored VERBATIM. MAME applies
 * ROM_REVERSE when it builds its big-endian region; a little-endian
 * `*(uint16*)` read of the raw chip bytes undoes precisely that, so the two
 * cancel and the file is written to flash unmodified. Confirmed on the real
 * dump: tk2j23c.bin starts ff 00 ee 62 00 00 a2 71, which reads back as
 * SSP=0x00FF62EE / PC=0x000071A2 -- a stack pointer inside work RAM and an
 * even PC, i.e. the pair cps1_rom_check_reset_vector() accepts.
 */
void cps1_m68k_map_prg_chip(uint32_t dest_offset, const uint8_t *data, uint32_t size);

/* Real 68000 reset: loads SSP from 0x000000 and PC from 0x000004 through
 * the map built above -- i.e. it actually honours the ROM's reset vector,
 * which the nine-opcode skeleton had no concept of. */
void cps1_m68k_reset(void);

/* Runs until at least `cycles` 68000 cycles have elapsed; returns the
 * cycle count actually consumed. */
uint32_t cps1_m68k_run(uint32_t cycles);

/* Raises/lowers a 68000 interrupt level (CPS-1 vblank is level 2,
 * autovectored -- see cps1_int_ack in cps1_m68k.c). */
void cps1_m68k_set_irq(unsigned level);

uint32_t cps1_m68k_get_pc(void);
uint32_t cps1_m68k_get_dreg(unsigned n);
uint32_t cps1_m68k_get_areg(unsigned n);

/* FNV-1a over D0-D7/A0-A7/PC/SR -- the comparison anchor for harnesses,
 * same role cps1_cpu68k_state_hash() had. */
uint16_t cps1_m68k_get_sr(void);

uint32_t cps1_m68k_state_hash(void);
