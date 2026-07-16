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

/* Boot-stall investigation (0716): who wrote $A12000 (the IFL2/INT2 doorbell)
 * and when. On real hardware this is MAIN's own VBlank ISR, pulsed every
 * frame; our run showed exactly one pulse ever (see segacd/CLAUDE.md /
 * session notes) — log (frame, PC) for every write so we can tell whether it
 * came from a VINT handler (expected to repeat) or a one-shot boot-time poke. */
#define SCD_DBG_A12000_LOG_N 8
extern int scd_dbg_frame;
uint32_t scd_dbg_a12000_frame[SCD_DBG_A12000_LOG_N], scd_dbg_a12000_pc[SCD_DBG_A12000_LOG_N];
int scd_dbg_a12000_n;
/* $FF800E/F comm-flag writer trace — which sub PC sets which bits, and when
 * (to find where a "TOC complete" flag would need to land). */
uint32_t scd_dbg_800f_pc[32]; uint8_t scd_dbg_800f_val[32]; int scd_dbg_800f_n;
/* regs[0x0e]/[0x0f] snapshot at the instant each $A12000 doorbell write fires. */
uint8_t scd_dbg_a12000_regef[8][2]; int scd_dbg_a12000_regef_n;
/* MAIN's own writes into regs[0x0e] (its half of the comm-flag word). */
uint32_t scd_dbg_a1200e_pc[32]; uint8_t scd_dbg_a1200e_val[32]; int scd_dbg_a1200e_n;
/* $A12001 (SRES/SBRQ) writes — which PC, when, what value. */
uint32_t scd_dbg_reg1_pc[32]; uint8_t scd_dbg_reg1_val[32]; uint32_t scd_dbg_reg1_frame[32]; int scd_dbg_reg1_n;
/* Boot-mode-4 gate: the VBlank ISR reads controller-1 ($A10003) into $FFFE20;
 * the disc-detect driver spins until $FE20's high nibble is nonzero. Track what
 * our emulation returns for $A10003 to tell a harness controller-stub artifact
 * (returns 0) from a real gap. */
uint32_t scd_dbg_a10003_reads; uint8_t scd_dbg_a10003_last;
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
    case 0x03: return SCD.s68k_regs[0x03] & 0x1f;    /* SUB's masked view of
                       * reg3 — pd_cd/memory.c s68k_reg_read16 case 2:
                       * `s68k_regs[3] & 0x1f`. */
    case 0x06: return 0x00;                          /* $FF8006: high byte of
                       * the CDC data-port word read is always 0 —
                       * pd_cd/memory.c s68k_reg_read16 case 6 assigns
                       * cdc_reg_r()'s byte result to the whole u32 d. */
    case 0x07: return segacd_cdc_reg_r();            /* $FF8007: CDC register[idx] */
    default:   return SCD.s68k_regs[reg];
    }
}

static unsigned int sub_ff_read16(unsigned int address)
{
    unsigned int off = address & 0xFFFF;
    if (off >= 0x8000 && (off & (SEGACD_GA_REGS - 1)) == 0x08) {
        /* $FF8008 host data port: a genuinely 16-bit register — must be
         * read ONCE (segacd_cdc_host_r has side effects: DAC advances,
         * DBC decrements), not composed from two independent 8-bit calls,
         * or a word access would double-consume. pd_cd/memory.c
         * s68k_reg_read16 case 8. Byte access to $FF8008/9 alone falls
         * through to the plain sub_ff_read8 path below (not a real
         * hardware access pattern; left unspecialized). */
        SGA_RD(0x08);
        return segacd_cdc_host_r(1);
    }
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

        if (reg == 0x02 || reg == 0x03) {
            /* $FF8002/3: Word-RAM DMNA/RET handshake (2M mode) + MODE/PM
             * bits. $FF8002 is byte-access only and aliases reg3 —
             * pd_cd/memory.c s68k_reg_write8 `case 2: a++`. SUB owns bits
             * 0 (RET), 2 (MODE), 3-4 (PM priority); bits 1(DMNA)/6-7(bank)
             * are preserved from the old value, then bits 0-1 are
             * recomposed from the dmna_ret_2m shadow so a SUB write can't
             * resurrect a DMNA the main already cleared. 1M-mode swap
             * semantics are NOT modeled (deferred — boot is 2M). */
            uint8_t dold = SCD.s68k_regs[0x03];
            uint8_t d    = (uint8_t)data & 0x1d;   /* SUB may supply bits 0,2,3,4 */
            d |= dold & 0xc2;                       /* preserve DMNA(1)/bank(6-7) */

            if (d & 0x01) {                 /* SUB sets RET: return Word-RAM to MAIN */
                SCD.dmna_ret_2m |= 0x01;
                SCD.dmna_ret_2m &= (uint8_t)~0x02;
            }

            if (d & 0x04) {
                /* 1M mode — TODO(ph2c): real bank-swap semantics if a
                 * 1M-mode title needs them; approximate the swap-complete
                 * DMNA clear only. */
                if ((d ^ dold) & 0x05) d &= (uint8_t)~0x02;
            } else {
                d = (uint8_t)((d & ~0x03) | SCD.dmna_ret_2m);
            }

            SCD.s68k_regs[0x03] = d;
            SCD.word_mode = (uint8_t)((d >> 2) & 1);
            segacd_poll_wake();
            return;
        }

        if (reg == 0x0e || reg == 0x0f) {
            /* $FF800E/F comm flag: SUB's half always targets regs[0x0f]
             * regardless of which byte of the word was addressed —
             * pd_cd/memory.c s68k_reg_write8 `case 0x0e: a++`. */
#ifdef SEGACD_GA_TRACE
            extern uint32_t scd_dbg_800f_pc[]; extern uint8_t scd_dbg_800f_val[]; extern int scd_dbg_800f_n;
            if (scd_dbg_800f_n < 32) { scd_dbg_800f_pc[scd_dbg_800f_n] = m68k.pc; scd_dbg_800f_val[scd_dbg_800f_n] = (uint8_t)data; scd_dbg_800f_n++; }
#endif
            SCD.s68k_regs[0x0f] = (uint8_t)data;
            return;
        }

        if (reg >= 0x10 && reg <= 0x1f) {
            /* $FF8010-1F comm command half: MAIN-owned. SUB writes are
             * ignored (pd_cd/memory.c: (a&0x1f0)==0x10 -> invalid write). */
            return;
        }

        switch (reg) {
        case 0x04:   /* CDC transfer destination select (bits 0-2). Also
                       * resets the DMA address registers 0xa/0xb —
                       * pd_cd/memory.c s68k_reg_write8 case 4. */
            SCD.s68k_regs[reg] = (uint8_t)(data & 0x07);
            SCD.s68k_regs[0x0a] = SCD.s68k_regs[0x0b] = 0;
            return;

        case 0x05:   /* CDC register-index latch — pd_cd/memory.c
                       * s68k_reg_write8 case 5. */
            SCD.s68k_regs[reg] = (uint8_t)(data & 0x1f);
            return;

        case 0x07:   /* CDC register data port write. */
            segacd_cdc_reg_w((uint8_t)data);
            return;

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

        case 0x66:   /* $FF8066 high byte: trace-vector base, no trigger yet —
                       * wait for the low byte ($FF8067) that completes the word
                       * write (the sub does a `move.w #imm,$FF8066`). */
            SCD.s68k_regs[reg] = (uint8_t)data;
            return;

        case 0x67:   /* Word-RAM graphics-transform TRIGGER: the low byte
                       * completes the $FF8066 word (trace-vector base). Run the
                       * ASIC render (segacd_gfx.c) NOW into Word-RAM, and if an
                       * op actually started, arm the level-1 (GFX) completion
                       * interrupt — segacd_run_sub delivers it frame-paced,
                       * gated on IEN1 ($FF8033 bit1), which the sub's animation
                       * loop (0x7a06) blocks on. gfx_start clears GRON
                       * ($FF8058 bit7) itself on completion. */
            SCD.s68k_regs[reg] = (uint8_t)data;
            {
                uint32_t base = ((uint32_t)SCD.s68k_regs[0x66] << 8) | SCD.s68k_regs[0x67];
                if (segacd_gfx_start(base))
                    SCD.gfx_op_armed = 1;
            }
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

    /* $0C0000-$0DFFFF : Word-RAM 1M-mode cell-arranged / linear bank view. The
     * boot-logo sub-BIOS sets up its stamp graphics here (sub PC 0x70f0: clears
     * $C0000.. and writes a header at $CE080). We do NOT model 1M banking yet
     * (see ph2b) — but leaving this NULL segfaults the sub the moment it touches
     * the region. DIAGNOSTIC: point it at a dedicated scratch bank so the sub can
     * run and we can observe the boot-mode progression; real 1M cell-mapping is a
     * follow-up once the reference confirms the sub is meant to be here now. */
    static uint8_t wram_1m_cell[2 * PAGE_SIZE];   /* $0C-$0D, 128 KB */
    map[0x0C].base = wram_1m_cell;
    map[0x0D].base = wram_1m_cell + PAGE_SIZE;

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
    if (!is_cd_ga(address)) {
        unsigned int v = orig_a1_read8(address);
#ifdef SEGACD_GA_TRACE
        /* Boot-mode-4 gate: the VBlank ISR reads controller-1 ($A10003) into
         * $FFFE20; the disc-detect driver spins until $FE20's high nibble is
         * nonzero. Log the first reads so we can see if the controller returns
         * idle (nonzero) or 0. */
        if ((address & 0xFFFFFF) == 0xA10003) {
            extern uint32_t scd_dbg_a10003_reads; extern uint8_t scd_dbg_a10003_last;
            scd_dbg_a10003_reads++; scd_dbg_a10003_last = (uint8_t)v;
        }
#endif
        return v;
    }
    GA_RD(address);
    unsigned int reg = address & (SEGACD_GA_REGS - 1);
    if (reg == 0x000)
        /* $A12000 low byte = IFL2 doorbell readback. Deliberately NOT the
         * shared regs[] array — see main_ga_write8 case reg==0 and segacd.h
         * SCD.ga_ifl2 for why $A12000 (main) and $FF8000 (sub: gate-array
         * version + LEDs) must not alias through the same byte. */
        return SCD.ga_ifl2 & 0x01;
    if (reg == 0x001)
        /* $A12001 = MAIN's SRES/SBRQ readback. Deliberately NOT the shared
         * regs[] array — see main_ga_write8 case reg==1 and segacd.h
         * SCD.main_busreq for why $A12001 (main) and $FF8001 (sub: LED +
         * soft-reset trigger, no persistent store on real hardware) must
         * not alias through the same byte. pd_cd/memory.c s68k_reg_read16
         * case 0 returns busreq packed into the low byte of the $A12000
         * word — this is that same value read as a lone byte. */
        return SCD.main_busreq & 0x03;
    if (reg == 0x003)
        /* MAIN's masked view of reg3: bits 3-5 are not visible to MAIN —
         * pd_cd/memory.c m68k_reg_read16 case 2: `s68k_regs[3] & 0xc7`. */
        return SCD.s68k_regs[0x03] & 0xc7;
    if (reg == 0x005)
        /* $A12005: low byte of the $A12004 word is always 0 for MAIN — it
         * would otherwise leak the SUB's CDC register-index latch, which
         * lives in the same shared regs[5] byte but is SUB-only.
         * pd_cd/memory.c m68k_reg_read16 case 4: `d = s68k_regs[4]<<8;`
         * (low byte never comes from regs[5]). */
        return 0x00;
    return SCD.s68k_regs[reg];
}
static unsigned int main_ga_read16(unsigned int address)
{
    if (!is_cd_ga(address)) return orig_a1_read16(address);
    unsigned int reg = address & (SEGACD_GA_REGS - 1);
    if (reg == 0x008) {
        /* $A12008 host data port: a genuinely 16-bit register — must be
         * read ONCE (segacd_cdc_host_r has side effects), not composed
         * from two independent 8-bit calls. pd_cd/memory.c
         * m68k_reg_read16 case 8. */
        GA_RD(address);
        return segacd_cdc_host_r(0);
    }
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
#ifdef SEGACD_GA_TRACE
        if (scd_dbg_a12000_n < SCD_DBG_A12000_LOG_N) {
            scd_dbg_a12000_frame[scd_dbg_a12000_n] = (uint32_t)scd_dbg_frame;
            scd_dbg_a12000_pc[scd_dbg_a12000_n] = m68k.pc;
            scd_dbg_a12000_n++;
        }
        extern uint8_t scd_dbg_a12000_regef[][2]; extern int scd_dbg_a12000_regef_n;
        if (scd_dbg_a12000_regef_n < 8) {
            scd_dbg_a12000_regef[scd_dbg_a12000_regef_n][0] = SCD.s68k_regs[0x0e];
            scd_dbg_a12000_regef[scd_dbg_a12000_regef_n][1] = SCD.s68k_regs[0x0f];
            scd_dbg_a12000_regef_n++;
        }
#endif
        return;
    }

    if (reg == 0x003) {
        /* $A12003: MAIN owns bits 6-7 (PRG bank) and requests bit1 (DMNA);
         * bits 2-4 (MODE/PM, owned by SUB) are preserved from the old value;
         * bits 0-1 (RET/DMNA) are recomposed from the persistent dmna_ret_2m
         * shadow so a MAIN write can't clobber a RET the sub already gave
         * back. 1M-mode swap semantics are NOT modeled (deferred — boot is
         * 2M). pd_cd/memory.c m68k_reg_write8 case 3. */
        uint8_t dold = SCD.s68k_regs[0x03];
        uint8_t d    = (uint8_t)data;

        if (d & 0x02) {                 /* MAIN requests DMNA: hand Word-RAM to SUB */
            SCD.dmna_ret_2m |= 0x02;
            SCD.dmna_ret_2m &= (uint8_t)~0x01;
        }

        if (dold & 0x04) {
            /* 1M mode — TODO(ph2c): real bank-swap semantics if a 1M-mode
             * title needs them; approximate the DMNA toggle only. */
            d ^= 0x02;
            d  = (uint8_t)((d & 0xc2) | (dold & 0x1f));
        } else {
            d = (uint8_t)((d & 0xc0) | (dold & 0x1c) | SCD.dmna_ret_2m);
        }

        SCD.s68k_regs[0x03] = d;
        SCD.prg_bank  = (uint8_t)((d >> 6) & 3);
        SCD.word_mode = (uint8_t)((d >> 2) & 1);
        segacd_poll_wake();
        return;
    }

    if (reg == 0x008) {
        /* $A12008: a MAIN write to the host data port acts the same as a
         * read of it (advances DAC/DBC the same way) — real hardware
         * quirk, pd_cd/memory.c m68k_reg_write8 case 8:
         * `(void) cdc_host_r(0); return;`. Nothing is stored. */
        (void)segacd_cdc_host_r(0);
        return;
    }
    if (reg == 0x009) {
        /* $A12009: not a valid MAIN write target either (reference falls
         * through to its "invalid write" branch — never stored); the low
         * byte of a 16-bit $A12008 write lands here and must not alias
         * anything. */
        return;
    }

    if (reg == 0x00e || reg == 0x00f) {
        /* $A1200E/F comm flag: MAIN's half always targets regs[0x0e]
         * regardless of which byte of the word was addressed —
         * pd_cd/memory.c m68k_reg_write8 `case 0x0f: a = 0x0e;`. */
        if (SCD.s68k_regs[0x0e] != (uint8_t)data) {
            SCD.s68k_regs[0x0e] = (uint8_t)data;
            segacd_poll_wake();
        }
#ifdef SEGACD_GA_TRACE
        if (scd_dbg_a1200e_n < 32) {
            scd_dbg_a1200e_pc[scd_dbg_a1200e_n] = m68k.pc;
            scd_dbg_a1200e_val[scd_dbg_a1200e_n] = (uint8_t)data;
            scd_dbg_a1200e_n++;
        }
#endif
        return;
    }

    if (reg >= 0x020 && reg <= 0x02f) {
        /* $A12020-2F comm status half: SUB-owned. MAIN writes are ignored,
         * matching real hardware/PicoDrive (falls through to the invalid-
         * write branch in m68k_reg_write8, i.e. never stored). */
        return;
    }

    if (reg == 0x001) {
        /* $A12001: bit0 SRES (0=sub in reset, 1=run), bit1 SBRQ (1=main holds
         * sub bus). Sub runs only when released AND its bus is granted.
         * Stored in SCD.main_busreq, NOT the shared regs[] array — see
         * segacd.h SCD.main_busreq for why aliasing it onto regs[1] let a
         * SUB write to its own $FF8001 permanently clobber this state. */
#ifdef SEGACD_GA_TRACE
        extern uint32_t scd_dbg_reg1_pc[]; extern uint8_t scd_dbg_reg1_val[]; extern uint32_t scd_dbg_reg1_frame[]; extern int scd_dbg_reg1_n;
        extern int scd_dbg_frame;
        if (scd_dbg_reg1_n < 32) {
            scd_dbg_reg1_pc[scd_dbg_reg1_n] = m68k.pc;
            scd_dbg_reg1_val[scd_dbg_reg1_n] = (uint8_t)data;
            scd_dbg_reg1_frame[scd_dbg_reg1_n] = (uint32_t)scd_dbg_frame;
            scd_dbg_reg1_n++;
        }
#endif
        SCD.main_busreq = (uint8_t)(data & 0x03);
        segacd_poll_wake();
        if ((data & 0x01) && !(data & 0x02)) {
            if (!SCD.sub_running) segacd_sub_release();
        } else {
            segacd_sub_hold();
        }
        return;
    }

    SCD.s68k_regs[reg] = (uint8_t)data;

    /* Any main write into the gate array can change what the sub polls. */
    segacd_poll_wake();
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
