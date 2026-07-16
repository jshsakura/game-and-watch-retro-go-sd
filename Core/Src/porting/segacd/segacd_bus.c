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

    /* Reset register ($FF8000-$FF8001). $FF8001 bit0 is the "peripheral not in
     * reset" ready flag — the sub-BIOS resets the CDC (writes bit0=0) then spins
     * on `btst #0,$8001` until it reads back 1. Hardware always reports ready, so
     * bit0 reads as 1 (matches PicoDrive s68k_reg_read16 case 0: `... | 1`).
     * $FF8000 high byte is the gate-array version (0). */
    switch (reg) {
    case 0x00: return SCD.s68k_regs[0x00] & 0x03;   /* version 0 + LED bits */
    case 0x01: return 0x01;                          /* CDC ready (not in reset) */
    default:   return SCD.s68k_regs[reg];            /* TODO ph3: CDC/CDD data */
    }
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
        unsigned int reg = off & (SEGACD_GA_REGS - 1);
        SGA_WR(off);

        switch (reg) {
        case 0x33:   /* IEN mask. IEN4 turning on while the CDD export is
                       * already armed ($FF8037 bit2) fires immediately rather
                       * than waiting up to one tick — pd_cd/memory.c:463-475
                       * `case 0x33`. */
            SCD.s68k_regs[reg] = (uint8_t)(data & 0x7e);
            if ((data & 0x10) && (SCD.s68k_regs[0x37] & 0x04))
                SCD.cdd_int_pending = 1;
            segacd_poll_wake();
            return;

        case 0x37:   /* CDD control. Bit2 ("HOCK"/export-armed) is set by the
                       * sub to request the periodic status transfer; setting
                       * it while IEN4 is already on fires immediately —
                       * pd_cd/memory.c:481-491 `case 0x37`. */
            SCD.s68k_regs[reg] = (uint8_t)(data & 0x07);
            if ((data & 0x04) && (SCD.s68k_regs[0x33] & 0x10))
                SCD.cdd_int_pending = 1;
            segacd_poll_wake();
            return;

        case 0x4b:   /* Command trigger: writing the 10th command byte fires
                       * decode+response. Cleared to 0 first, matching real
                       * hardware/pd_cd/memory.c:492-510 `case 0x4b`. */
            SCD.s68k_regs[reg] = 0;
            segacd_cdd_command();
            return;

        default:
            SCD.s68k_regs[reg] = (uint8_t)data;
            return;
        }
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
uint32_t scd_dbg_prgwin_w;              /* count of main-CPU writes into the PRG window */
uint8_t  scd_dbg_prg_written[0x5800];   /* coverage of the sub-BIOS region by main writes */
uint32_t scd_dbg_wpc[64]; int scd_dbg_wpc_n;   /* distinct main PCs that store into PRG */
int scd_dbg_first_store_seen; uint32_t scd_dbg_first_a0, scd_dbg_first_a1, scd_dbg_first_ea;
extern m68ki_cpu_core m68k;             /* to read the writer's PC (locate the decompressor) */
#endif
static void main_prgwin_write8(unsigned int address, unsigned int data)
{
    unsigned int off = (address & 0x1FFFF) + (unsigned)SCD.prg_bank * 0x20000;
#ifdef SEGACD_GA_TRACE
    scd_dbg_prgwin_w++;
    { unsigned physoff = (off ^ 1) & (SEGACD_PRG_RAM_SIZE - 1);
      if (physoff < sizeof(scd_dbg_prg_written)) scd_dbg_prg_written[physoff] = 1; }
    { unsigned pc = m68k.pc; int seen = 0;
      for (int i = 0; i < scd_dbg_wpc_n; i++) if (scd_dbg_wpc[i] == pc) { seen = 1; break; }
      if (!seen && scd_dbg_wpc_n < 64) scd_dbg_wpc[scd_dbg_wpc_n++] = pc; }
    /* Capture A0(src)/A1(dst) at the LZSS decompressor's first store only
     * (PC 0x926 = literal copy, 0x988 = back-ref copy), not the PRG-clear loop. */
    if (!scd_dbg_first_store_seen && (m68k.pc == 0x926 || m68k.pc == 0x988)) {
        scd_dbg_first_store_seen = 1;
        scd_dbg_first_a0 = m68k.dar[8];
        scd_dbg_first_a1 = m68k.dar[9];
        scd_dbg_first_ea = address;
    }
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
    unsigned int reg = address & (SEGACD_GA_REGS - 1);
    if (reg == 0x000)
        /* $A12000 low byte = IFL2 doorbell readback. Deliberately NOT the
         * shared regs[] array — see main_ga_write8 case reg==0 and segacd.h
         * SCD.ga_ifl2 for why $A12000 (main) and $FF8000 (sub: gate-array
         * version + LEDs) must not alias through the same byte. */
        return SCD.ga_ifl2 & 0x01;
    return SCD.s68k_regs[reg];
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

    if (reg == 0x000) {
        /* $A12000 bit0 = IFL2 doorbell to the sub (level-2 IRQ, "INT2").
         * Real hardware/PicoDrive gate this on the sub's IEN2 bit AT WRITE
         * TIME (pd_cd/memory.c:171-182 `case 0: ... if (d && IEN2) ...`) —
         * a pulse sent before the sub has armed IEN2 is simply lost. We
         * defer the IEN2 check to delivery time instead (segacd_run_sub in
         * segacd_engine.c), so an early doorbell stays "pending" until the
         * sub catches up and enables IEN2, rather than requiring the main
         * BIOS to re-pulse it — this is the sub-BIOS's actual boot order
         * (it enables interrupts only after the doorbell already arrived). */
        SCD.ga_ifl2 = (uint8_t)(data & 0x01);
        segacd_poll_wake();
        return;
    }

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
