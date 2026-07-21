/* CPS-1 68000: Musashi wired to the CPS-1 memory map. See cps1_m68k.h for
 * why this replaced the nine-opcode cps1_cpu68k.c skeleton, what it costs,
 * and the byte-swap contract for base-mapped regions. */
#include <stddef.h>
#include <stdio.h> /* m68k.h declares save/load state with FILE* */

#include "m68k.h"
#include "cps1_m68k.h"

/* Musashi's CPU state. m68kcpu.c does `#define m68ki_cpu m68k` and declares
 * `m68ki_cpu_core m68k;` -- the page table lives at m68k.memory_map[256]. */
extern m68ki_cpu_core m68k;

#define CPS1_M68K_PAGE_SIZE   0x10000u
#define CPS1_M68K_PAGE_COUNT  256u

#define CPS1_PRG_PAGE_FIRST   0x00u
#define CPS1_PRG_PAGE_LIMIT   0x40u  /* 0x000000-0x3FFFFF, 4 MB */
#define CPS1_IO_PAGE          0x80u  /* 0x800000-0x80FFFF, CPS-A/CPS-B */
#define CPS1_GFXRAM_PAGE_FIRST 0x90u /* 0x900000-0x92FFFF, 192 KB */
#define CPS1_GFXRAM_PAGE_COUNT 3u
#define CPS1_WRAM_PAGE        0xFFu  /* 0xFF0000-0xFFFFFF, 64 KB */

static const cps1_m68k_io_t *s_io;

/* ---- open bus: unmapped reads float high, writes are dropped ---- */

static unsigned int cps1_openbus_read8(unsigned int address)
{
    (void)address;
    return 0xFFu;
}

static unsigned int cps1_openbus_read16(unsigned int address)
{
    (void)address;
    return 0xFFFFu;
}

static void cps1_openbus_write8(unsigned int address, unsigned int data)
{
    (void)address; (void)data;
}

static void cps1_openbus_write16(unsigned int address, unsigned int data)
{
    (void)address; (void)data;
}

/* ---- I/O regions (gfxram, CPS-A/CPS-B): word-granular, side effects ----
 *
 * cps1_core.c's bus is word-only (palette conversion keys off whole words),
 * so byte accesses are synthesized: read8 picks the half of the containing
 * word, write8 is a read-modify-write of that word. CPS-1 code addresses
 * these regions as words in practice; doing it this way keeps ONE
 * implementation of the register/palette semantics instead of a second,
 * silently-diverging byte-wide copy. */

static unsigned int cps1_io_read8(unsigned int address)
{
    if (s_io == NULL || s_io->read16 == NULL)
        return 0xFFu;
    uint16_t word = s_io->read16(address & ~1u);
    return (address & 1u) ? (word & 0xFFu) : (word >> 8);
}

static unsigned int cps1_io_read16(unsigned int address)
{
    if (s_io == NULL || s_io->read16 == NULL)
        return 0xFFFFu;
    return s_io->read16(address & ~1u);
}

static void cps1_io_write8(unsigned int address, unsigned int data)
{
    if (s_io == NULL || s_io->write16 == NULL)
        return;
    uint32_t even = address & ~1u;
    uint16_t word = (s_io->read16 != NULL) ? s_io->read16(even) : 0;
    if (address & 1u)
        word = (uint16_t)((word & 0xFF00u) | (data & 0xFFu));
    else
        word = (uint16_t)((word & 0x00FFu) | ((data & 0xFFu) << 8));
    s_io->write16(even, word);
}

static void cps1_io_write16(unsigned int address, unsigned int data)
{
    if (s_io == NULL || s_io->write16 == NULL)
        return;
    s_io->write16(address & ~1u, (uint16_t)data);
}

/* Musashi's M68K_EMULATE_INT_ACK is ON and m68kcpu.c calls this symbol
 * (declared there as `extern int vdp_68k_irq_ack(int)` for Genesis). CPS-1
 * interrupts are autovectored -- the 68000 supplies the vector itself, the
 * board does not drive one onto the bus. Providing it here is also what
 * keeps cps1's overlay from binding to gwenesis's copy: without a
 * definition of its own, the linker would silently alias another core's
 * (CLAUDE.md, "Cores are overlays -- a missing symbol does not fail the
 * link, it aliases"). */
int vdp_68k_irq_ack(int int_level)
{
    (void)int_level;
    return M68K_INT_ACK_AUTOVECTOR;
}

static void cps1_map_page_io(unsigned page)
{
    m68k.memory_map[page].base    = NULL;
    m68k.memory_map[page].read8   = cps1_io_read8;
    m68k.memory_map[page].read16  = cps1_io_read16;
    m68k.memory_map[page].write8  = cps1_io_write8;
    m68k.memory_map[page].write16 = cps1_io_write16;
}

static void cps1_map_page_openbus(unsigned page)
{
    m68k.memory_map[page].base    = NULL;
    m68k.memory_map[page].read8   = cps1_openbus_read8;
    m68k.memory_map[page].read16  = cps1_openbus_read16;
    m68k.memory_map[page].write8  = cps1_openbus_write8;
    m68k.memory_map[page].write16 = cps1_openbus_write16;
}

/* base-mapped: Musashi dereferences `base` directly (no callback), which is
 * what makes ROM cost zero RAM (base can point into XIP flash) and WRAM
 * cost one pointer. NULL callbacks are the signal to take that path. */
static void cps1_map_page_base(unsigned page, uint8_t *base)
{
    m68k.memory_map[page].base    = base;
    m68k.memory_map[page].read8   = NULL;
    m68k.memory_map[page].read16  = NULL;
    m68k.memory_map[page].write8  = NULL;
    m68k.memory_map[page].write16 = NULL;
}

void cps1_m68k_init(const uint8_t *prg, uint32_t prg_size, uint8_t *wram,
                     const cps1_m68k_io_t *io)
{
    s_io = io;

    m68k_init();

    for (unsigned page = 0; page < CPS1_M68K_PAGE_COUNT; page++)
        cps1_map_page_openbus(page);

    /* Program ROM: whole 64 KB pages only, capped at the 4 MB the map
     * reserves. A short ROM leaves its tail pages as open bus rather than
     * wrapping -- a wrapped alias would read as valid code and hide the
     * mistake. */
    uint32_t prg_pages = (prg != NULL) ? (prg_size / CPS1_M68K_PAGE_SIZE) : 0;
    if (prg_pages > (CPS1_PRG_PAGE_LIMIT - CPS1_PRG_PAGE_FIRST))
        prg_pages = CPS1_PRG_PAGE_LIMIT - CPS1_PRG_PAGE_FIRST;
    for (uint32_t i = 0; i < prg_pages; i++) {
        /* cast away const: Musashi's base is a non-const pointer, but every
         * ROM page is mapped read-callback-free and never written -- the
         * write path for these pages is the open-bus stub below. */
        cps1_map_page_base(CPS1_PRG_PAGE_FIRST + i,
                            (uint8_t *)(uintptr_t)(prg + (size_t)i * CPS1_M68K_PAGE_SIZE));
    }

    cps1_map_page_io(CPS1_IO_PAGE);
    for (unsigned i = 0; i < CPS1_GFXRAM_PAGE_COUNT; i++)
        cps1_map_page_io(CPS1_GFXRAM_PAGE_FIRST + i);

    if (wram != NULL)
        cps1_map_page_base(CPS1_WRAM_PAGE, wram);
}

void cps1_m68k_reset(void)
{
    m68k_pulse_reset();
}

uint32_t cps1_m68k_run(uint32_t cycles)
{
    uint32_t before = m68k.cycles;
    m68k_run(before + cycles);
    return m68k.cycles - before;
}

void cps1_m68k_set_irq(unsigned level)
{
    m68k_set_irq(level);
}

uint32_t cps1_m68k_get_pc(void)
{
    return m68k_get_reg(M68K_REG_PC);
}

uint32_t cps1_m68k_get_dreg(unsigned n)
{
    return m68k_get_reg((m68k_register_t)(M68K_REG_D0 + (n & 7)));
}

uint32_t cps1_m68k_get_areg(unsigned n)
{
    return m68k_get_reg((m68k_register_t)(M68K_REG_A0 + (n & 7)));
}

uint16_t cps1_m68k_get_sr(void)
{
    return (uint16_t)m68k_get_reg(M68K_REG_SR);
}

uint32_t cps1_m68k_state_hash(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 8; i++) { h ^= cps1_m68k_get_dreg(i); h *= 16777619u; }
    for (unsigned i = 0; i < 8; i++) { h ^= cps1_m68k_get_areg(i); h *= 16777619u; }
    h ^= cps1_m68k_get_pc();                       h *= 16777619u;
    h ^= m68k_get_reg(M68K_REG_SR);          h *= 16777619u;
    return h;
}
