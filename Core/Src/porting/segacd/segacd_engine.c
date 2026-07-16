/* Sega/Mega CD — dual-68K engine on gwenesis's Musashi.
 *
 * KEYSTONE of the port. gwenesis keeps all 68K state in one global
 * `m68ki_cpu_core m68k`, and that struct CONTAINS the memory_map. So a second
 * 68000 is just a second snapshot of that struct: to run the sub-CPU we save
 * the main context, load the sub context (whose memory_map points at PRG/Word
 * RAM instead of the cartridge), run, then restore main. This is exactly
 * PicoDrive's m68k_get/set_context model (pd_cd/sek.c), done by struct copy
 * because this stripped gwenesis Musashi has no get/set_context helpers.
 *
 * Cost of the swap: two struct copies per timeslice — negligible vs the
 * instructions run inside. RAM cost of the second CPU: sizeof(m68ki_cpu_core),
 * ~a few hundred bytes. The heavy RAM (PRG/Word/PCM) is separate, in SCD.
 *
 * Phase 1: engine + RAM alloc + context swap wired. The sub-CPU's memory_map
 * setup, the gate-array bus, CDC/CDD, ASIC and PCM are later phases (see
 * porting/segacd/CLAUDE.md). Functions that need those are marked TODO and
 * kept as safe no-ops so the tree links.
 */
#include <string.h>
#include "segacd.h"
#include "gwenesis_bus.h"

/* device allocators (ahb_malloc = AHB SRAM, ram_malloc = AXI RAM_EMU pool) */
extern void *ram_malloc(size_t);
extern void *ahb_malloc(size_t);

/* gwenesis global main-CPU context and its runner (m68k_run returns void;
 * cycles consumed are tracked in the m68k.cycles field). */
extern m68ki_cpu_core m68k;
extern void m68k_run(unsigned int cycles);
extern void m68k_init(void);
extern void m68k_pulse_reset(void);
extern void m68k_set_irq(unsigned int int_level);

segacd_state SCD;

#ifdef SEGACD_GA_TRACE
int scd_dbg_sum_seen; uint32_t scd_dbg_sum_a0, scd_dbg_sum_d0, scd_dbg_cmp_d0, scd_dbg_cmp_d1, scd_dbg_maxpc;
#endif

/* Saved MAIN context while the sub-CPU is running. Static, one level of
 * nesting only — the sub never re-enters the main. */
static m68ki_cpu_core s_main_saved;

void segacd_init(void)
{
    memset(&SCD, 0, sizeof(SCD));

    /* Resident CD RAM. On device these land in specific banks via the overlay;
     * ram_malloc/ahb_malloc pick the pool. PRG+Word in AXI, PCM in AHB. */
    SCD.prg_ram  = ram_malloc(SEGACD_PRG_RAM_SIZE);
    SCD.word_ram = ram_malloc(SEGACD_WORD_RAM_SIZE);
    SCD.pcm_ram  = ahb_malloc(SEGACD_PCM_RAM_SIZE);

    SCD.word_mode  = 0;   /* 2M mode at reset */
    SCD.word_owner = 0;   /* main-CPU owns Word-RAM after reset */
    SCD.sub_running = 0;  /* sub held in reset until BIOS releases it */

    segacd_reset();
}

void segacd_reset(void)
{
    if (SCD.prg_ram)  memset(SCD.prg_ram,  0, SEGACD_PRG_RAM_SIZE);
    if (SCD.word_ram) memset(SCD.word_ram, 0, SEGACD_WORD_RAM_SIZE);
    if (SCD.pcm_ram)  memset(SCD.pcm_ram,  0, SEGACD_PCM_RAM_SIZE);
    memset(SCD.s68k_regs, 0, sizeof(SCD.s68k_regs));

    /* Build the sub-CPU context by INHERITING the already-initialized main
     * context, then overriding its memory_map for the sub's address space.
     * Inheriting matters: m68ki_cpu_core holds CPU config the sub needs —
     * cpu_type, sr_mask, and the callback function pointers. With
     * M68K_EMULATE_INT_ACK=OPT_ON the interrupt path calls int_ack_callback on
     * EVERY interrupt; a zeroed context makes it NULL and the sub jumps to 0 the
     * first time it takes an IRQ. The registers/PC/cycles here are throwaway —
     * m68k_pulse_reset() sets SP/PC from the reset vectors when the sub is
     * released, and each timeslice rebases cycles to 0. */
    memcpy(&SCD.sub_ctx, &m68k, sizeof(SCD.sub_ctx));
    segacd_sub_build_memory_map();
}

/* Release the sub-68K from reset: swap its context in, pulse-reset so it fetches
 * SP/PC from the reset vectors the BIOS wrote into PRG-RAM, then start it.
 * Called from the gate-array handler when the main-CPU clears SRES (and the sub
 * bus is granted). */
void segacd_sub_release(void)
{
    memcpy(&s_main_saved, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &SCD.sub_ctx, sizeof(m68ki_cpu_core));
    m68k_pulse_reset();
    memcpy(&SCD.sub_ctx, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &s_main_saved, sizeof(m68ki_cpu_core));
    SCD.sub_running = 1;
    SCD.sub_idle = 0;
}

/* Hold the sub-68K in reset / bus-request (main sets SRES or takes the bus). */
void segacd_sub_hold(void)
{
    SCD.sub_running = 0;
}

/* Run the sub-68K up to a cycle target, with its context swapped in.
 * Returns cycles actually consumed. No-op while the sub is held or idle. */
int segacd_run_sub(int cycle_target)
{
    if (!SCD.sub_running)
        return 0;
    /* An interrupt must wake a spin-idled sub; only skip when truly idle. */
    if (SCD.sub_idle && !SCD.cdd_int_pending)
        return 0;
    SCD.sub_idle = 0;

    /* save main -> load sub */
    memcpy(&s_main_saved, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &SCD.sub_ctx, sizeof(m68ki_cpu_core));

    /* Deliver the CDD interrupt (level 4). The sub-CPU BIOS is interrupt-driven
     * — after init it waits for the periodic CDD IRQ to run its drive state
     * machine. Without this the sub sits idle forever. Masked by the sub's SR /
     * gate-array IEN, so it's a no-op until the sub enables it. */
    if (SCD.cdd_int_pending) {
        SCD.cdd_int_pending = 0;
        m68k_set_irq(4);
    }

    /* m68k_run() takes an ABSOLUTE cycle target, and the sub's cycle counter
     * persists in sub_ctx across timeslices. If we passed a fixed target the
     * sub would run one timeslice's worth of cycles ONCE and then every later
     * call would find m68k.cycles already past the target and do nothing — the
     * sub would freeze mid-instruction (it froze inside the BIOS self-checksum
     * sum loop at PC 0x2e0). Rebase to 0 each slice, exactly as the main frame
     * loop does with `m68k.cycles -= system_clock`. */
    m68k.cycles = 0;
    m68k_run((unsigned int)cycle_target);
    int used = m68k.cycles;
#ifdef SEGACD_GA_TRACE
    /* Track the furthest PC the sub reaches past the self-checksum (which PASSES:
     * sum 0x200..0x5800 == 0xe9bb). */
    extern uint32_t scd_dbg_maxpc;
    if (m68k.pc > scd_dbg_maxpc && m68k.pc < 0x6000) scd_dbg_maxpc = m68k.pc;
#endif

    /* save sub -> restore main */
    memcpy(&SCD.sub_ctx, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &s_main_saved, sizeof(m68ki_cpu_core));

    return used;
}
