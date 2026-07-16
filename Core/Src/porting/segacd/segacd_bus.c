/* Sega/Mega CD bus wiring — phase 2.
 *
 * Two address spaces:
 *   SUB-68K  (SCD.sub_ctx.memory_map):
 *     $000000-$07FFFF  PRG-RAM   512 KB (direct, 8 pages)
 *     $080000-$0BFFFF  Word-RAM  256 KB (2M mode, direct — arbitration TODO ph2b)
 *     $FF0000-$FF7FFF  PCM window (handlers — RF5C164, ph4)
 *     $FF8000-$FFFFFF  sub gate array / CDC / CDD regs (handlers)
 *   MAIN-68K  (extends gwenesis m68k.memory_map after cart load):
 *     $020000-$03FFFF  PRG-RAM 128 KB window (bank BK0/BK1)
 *     $200000-$23FFFF  Word-RAM (2M) / bank (1M)
 *     $A12000-$A120FF  gate array registers
 *
 * gwenesis page granularity: memory_map[256], one per 64 KB. base != NULL means
 * direct base[addr & 0xFFFF]; base == NULL routes to the read/write handlers.
 * The direct-base regions set base per-page to the right sub-offset so the
 * `& 0xFFFF` index is correct across a multi-page array.
 */
#include <string.h>
#include "segacd.h"
#include "gwenesis_bus.h"

extern m68ki_cpu_core m68k;

#define PAGE_SHIFT 16
#define PAGE_SIZE  0x10000
#define PAGE(addr) (((addr) >> PAGE_SHIFT) & 0xFF)

/* idle-skip: after this many unchanged reads of the same GA status reg, the
 * sub-68K is judged to be spin-waiting and its timeslices are skipped until a
 * write re-arms it. Small so it triggers fast; the wake is exact. */
#define SEGACD_POLL_THRESHOLD 64

/* A write that could change what the sub is polling wakes it. */
void segacd_poll_wake(void)
{
    SCD.sub_idle   = 0;
    SCD.poll_count = 0;
}

/* ---- gate-array access trace (boot debugging; harness-first) ---- */
#ifdef SEGACD_GA_TRACE
uint32_t scd_ga_rd[SEGACD_GA_REGS], scd_ga_wr[SEGACD_GA_REGS];       /* main side */
uint32_t scd_sga_rd[SEGACD_GA_REGS], scd_sga_wr[SEGACD_GA_REGS];     /* sub side  */
#define GA_RD(reg)  (scd_ga_rd[(reg) & (SEGACD_GA_REGS-1)]++)
#define GA_WR(reg)  (scd_ga_wr[(reg) & (SEGACD_GA_REGS-1)]++)
#define SGA_RD(reg) (scd_sga_rd[(reg) & (SEGACD_GA_REGS-1)]++)
#define SGA_WR(reg) (scd_sga_wr[(reg) & (SEGACD_GA_REGS-1)]++)
#else
#define GA_RD(reg) ((void)0)
#define GA_WR(reg) ((void)0)
#define SGA_RD(reg) ((void)0)
#define SGA_WR(reg) ((void)0)
#endif

/* ---- sub-CPU $FF0000 page: PCM (low) + gate array/CDC/CDD (high) ---- */

static unsigned int sub_ff_read8(unsigned int address)
{
    unsigned int off = address & 0xFFFF;
    if (off < 0x8000)                       /* $FF0000-$FF7FFF: PCM window */
        return SCD.pcm_ram[off & (SEGACD_PCM_RAM_SIZE - 1)];  /* TODO ph4: banked */

    /* $FF8000+: gate array / CDC / CDD. Track repeated reads of the same status
     * reg to detect a spin-wait and skip the sub's timeslices (idle-skip). */
    uint8_t reg = (uint8_t)(off & (SEGACD_GA_REGS - 1));
    SGA_RD(reg);
    if (reg == SCD.poll_reg) {
        if (SCD.poll_count < 0xFFFF && ++SCD.poll_count >= SEGACD_POLL_THRESHOLD)
            SCD.sub_idle = 1;
    } else {
        SCD.poll_reg = reg;
        SCD.poll_count = 0;
    }
    return SCD.s68k_regs[reg];                                /* TODO ph3: CDC/CDD */
}

static unsigned int sub_ff_read16(unsigned int address)
{
    return (sub_ff_read8(address) << 8) | sub_ff_read8(address + 1);
}

static void sub_ff_write8(unsigned int address, unsigned int data)
{
    unsigned int off = address & 0xFFFF;
    if (off < 0x0020) {                    /* $FF0001-$FF001F: RF5C164 registers */
        segacd_pcm_write(off >> 1, data);
    } else if (off >= 0x2000 && off < 0x4000) {   /* $FF2000-$FF3FFF: 4 KB wave-RAM window */
        unsigned int a = (unsigned)SCD.pcm.bank * 0x1000 + (off & 0x0FFF);
        SCD.pcm_ram[a & (SEGACD_PCM_RAM_SIZE - 1)] = (uint8_t)data;
    } else if (off >= 0x8000) {            /* $FF8000+: gate array / CDC / CDD */
        SGA_WR(off);
        SCD.s68k_regs[off & (SEGACD_GA_REGS - 1)] = (uint8_t)data;
    }
}

static void sub_ff_write16(unsigned int address, unsigned int data)
{
    sub_ff_write8(address, data >> 8);
    sub_ff_write8(address + 1, data & 0xFF);
}

/* Build the sub-CPU's 256-entry memory_map into SCD.sub_ctx. */
void segacd_sub_build_memory_map(void)
{
    cpu_memory_map *map = SCD.sub_ctx.memory_map;
    memset(map, 0, sizeof(SCD.sub_ctx.memory_map));

    /* $000000-$07FFFF : PRG-RAM, 8 direct pages */
    for (int p = 0; p < 8; p++)
        map[p].base = SCD.prg_ram + p * PAGE_SIZE;

    /* $080000-$0BFFFF : Word-RAM (2M mode, sub owns), 4 direct pages.
     * TODO(ph2b): honor word_mode/word_owner — NULL out + handler when main owns. */
    for (int p = 0; p < 4; p++)
        map[0x08 + p].base = SCD.word_ram + p * PAGE_SIZE;

    /* $FF0000-$FFFFFF : PCM + gate array, handler page */
    map[0xFF].base   = NULL;
    map[0xFF].read8  = sub_ff_read8;
    map[0xFF].read16 = sub_ff_read16;
    map[0xFF].write8 = sub_ff_write8;
    map[0xFF].write16= sub_ff_write16;
}

/* ---- main-CPU view of CD space (handlers; PRG window is bank-selected) ----
 *
 * BYTE ORDER (critical): the sub-68K reaches PRG-RAM through a *direct* memory_map
 * base (segacd_sub_build_memory_map), so it uses gwenesis's byte-swapped storage
 * convention on this LSB_FIRST host — READ_BYTE/WRITE_BYTE index base[addr^1], and
 * 16-bit accesses are *(uint16*)(base+addr) (little-endian read of a pair-swapped
 * word). The main-68K reaches the same PRG-RAM through *these handlers* (the bank-
 * selected $020000 window), so they MUST store in the identical swapped layout, or
 * the sub-BIOS the main copies in comes out with every word's bytes transposed and
 * the sub executes garbage (it did: sub wandered to PC=0x7762 fetching data).
 *
 * Fix: the 8-bit accessors apply the same `^1` the sub's READ_BYTE/WRITE_BYTE do;
 * the 16-bit wrappers compose two 8-bit accesses in big-endian order, which — with
 * the `^1` in place — writes/reads the little-endian pair the sub's direct base
 * expects. Word-RAM needs no such handler: main and sub both see it via direct base
 * (same convention), so it is already consistent. */
static unsigned int main_prgwin_read8(unsigned int address)
{
    unsigned int off = (address & 0x1FFFF) + (unsigned)SCD.prg_bank * 0x20000;
    return SCD.prg_ram[(off ^ 1) & (SEGACD_PRG_RAM_SIZE - 1)];
}
static unsigned int main_prgwin_read16(unsigned int address)
{
    return (main_prgwin_read8(address) << 8) | main_prgwin_read8(address + 1);
}
#ifdef SEGACD_GA_TRACE
uint32_t scd_dbg_prgwin_w;   /* count of main-CPU writes into the PRG window */
#endif
static void main_prgwin_write8(unsigned int address, unsigned int data)
{
    unsigned int off = (address & 0x1FFFF) + (unsigned)SCD.prg_bank * 0x20000;
#ifdef SEGACD_GA_TRACE
    scd_dbg_prgwin_w++;
#endif
    SCD.prg_ram[(off ^ 1) & (SEGACD_PRG_RAM_SIZE - 1)] = (uint8_t)data;
}
static void main_prgwin_write16(unsigned int address, unsigned int data)
{
    main_prgwin_write8(address, data >> 8);
    main_prgwin_write8(address + 1, data & 0xFF);
}

/* Page 0xA1 holds MUCH more than the CD gate array ($A12000-$A120FF): the Z80
 * bus/reset regs ($A11100/$A11200), controller I/O ($A10xxx), cart regs
 * ($A13xxx), TMSS ($A14xxx). gwenesis dispatches all of those in its own
 * page-0xA1 handler; we save those pointers and chain to them for anything
 * outside the gate array, or the BIOS spins forever polling $A11100 (Z80
 * BUSREQ) that our handler was wrongly swallowing. */
static unsigned int (*orig_a1_read8)(unsigned int);
static unsigned int (*orig_a1_read16)(unsigned int);
static void         (*orig_a1_write8)(unsigned int, unsigned int);
static void         (*orig_a1_write16)(unsigned int, unsigned int);

static inline int is_cd_ga(unsigned int address)
{
    unsigned int a = address & 0xFFFF;   /* gate array = $A12000-$A120FF */
    return a >= 0x2000 && a <= 0x20FF;
}

static unsigned int main_ga_read8(unsigned int address)
{
    if (!is_cd_ga(address)) return orig_a1_read8(address);
    GA_RD(address);
    return SCD.s68k_regs[address & (SEGACD_GA_REGS - 1)];   /* TODO ph3: real GA */
}
static unsigned int main_ga_read16(unsigned int address)
{
    if (!is_cd_ga(address)) return orig_a1_read16(address);
    return (main_ga_read8(address) << 8) | main_ga_read8(address + 1);
}
static void main_ga_write16(unsigned int address, unsigned int data);
static void main_ga_write8(unsigned int address, unsigned int data)
{
    if (!is_cd_ga(address)) { orig_a1_write8(address, data); return; }
    unsigned int reg = address & (SEGACD_GA_REGS - 1);
    GA_WR(address);
    SCD.s68k_regs[reg] = (uint8_t)data;

    /* Any main write into the gate array can change what the sub polls. */
    segacd_poll_wake();

    switch (address & 0xFFF) {
    case 0x001:
        /* $A12001: bit0 SRES (0=sub in reset, 1=run), bit1 SBRQ (1=main holds
         * sub bus). Sub runs only when released AND its bus is granted. */
        if ((data & 0x01) && !(data & 0x02)) {
            if (!SCD.sub_running) segacd_sub_release();
        } else {
            segacd_sub_hold();
        }
        break;
    case 0x003:
        /* PRG-RAM 128 KB bank select (BK0/BK1) + Word-RAM mode/owner bits. */
        SCD.prg_bank  = (uint8_t)((data >> 6) & 3);
        SCD.word_mode = (uint8_t)((data >> 2) & 1);   /* DMNA/MODE — refine ph2b */
        break;
    }
}
static void main_ga_write16(unsigned int address, unsigned int data)
{
    if (!is_cd_ga(address)) { orig_a1_write16(address, data); return; }
    main_ga_write8(address, data >> 8);
    main_ga_write8(address + 1, data & 0xFF);
}

/* Map the region BIOS as the MAIN-CPU boot ROM at $000000-$01FFFF (2 pages).
 * `bios` is read-only — a flash-XIP pointer on device (0 RAM), or a RAM buffer.
 * The Mega CD boots the main 68K from BIOS, not the cartridge. */
void segacd_map_bios(const uint8_t *bios)
{
    if (!bios) return;
    for (int p = 0x00; p <= 0x01; p++) {
        m68k.memory_map[p].base   = (unsigned char *)bios + p * PAGE_SIZE;
        m68k.memory_map[p].read8  = NULL;
        m68k.memory_map[p].read16 = NULL;
        /* BIOS is read-only: leave write handlers NULL (writes ignored). */
        m68k.memory_map[p].write8 = NULL;
        m68k.memory_map[p].write16= NULL;
    }
}

/* Patch the MAIN gwenesis memory_map for CD regions. Call AFTER
 * load_cartridge()/gwenesis_bus_init_memory_map() so we override cart pages. */
void segacd_main_map_cd_space(void)
{
    cpu_memory_map *map = m68k.memory_map;

    /* $020000-$03FFFF : PRG-RAM 128 KB window (2 pages) */
    for (int p = 0x02; p <= 0x03; p++) {
        map[p].base   = NULL;
        map[p].read8  = main_prgwin_read8;
        map[p].read16 = main_prgwin_read16;
        map[p].write8 = main_prgwin_write8;
        map[p].write16= main_prgwin_write16;
    }

    /* $200000-$23FFFF : Word-RAM (2M mode, main owns after reset), 4 pages.
     * TODO(ph2b): arbitration — NULL + handler when sub owns. */
    for (int p = 0; p < 4; p++)
        map[0x20 + p].base = SCD.word_ram + p * PAGE_SIZE;

    /* $A12000 page : gate array registers. Page $A1 also carries Z80/io/cart/
     * TMSS, all handled by gwenesis's own dispatcher — save it and chain to it
     * for everything outside $A12000-$A120FF (see is_cd_ga / main_ga_read8). */
    if (map[0xA1].read8 != main_ga_read8) {  /* don't capture our own on re-map */
        orig_a1_read8  = map[0xA1].read8;
        orig_a1_read16 = map[0xA1].read16;
        orig_a1_write8 = map[0xA1].write8;
        orig_a1_write16= map[0xA1].write16;
    }
    map[0xA1].base   = NULL;
    map[0xA1].read8  = main_ga_read8;
    map[0xA1].read16 = main_ga_read16;
    map[0xA1].write8 = main_ga_write8;
    map[0xA1].write16= main_ga_write16;
}
