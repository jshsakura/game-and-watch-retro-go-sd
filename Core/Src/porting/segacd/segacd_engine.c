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

segacd_state SCD;

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

    /* Build the sub-CPU context: zeroed core, then its memory_map over
     * PRG-RAM / Word-RAM / gate array. Sub stays held (sub_running = 0) until
     * the BIOS release path (phase 3) sets its reset vector and starts it. */
    memset(&SCD.sub_ctx, 0, sizeof(SCD.sub_ctx));
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
    if (!SCD.sub_running || SCD.sub_idle)
        return 0;

    /* save main -> load sub */
    memcpy(&s_main_saved, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &SCD.sub_ctx, sizeof(m68ki_cpu_core));

    int before = m68k.cycles;
    m68k_run((unsigned int)cycle_target);
    int used = m68k.cycles - before;

    /* save sub -> restore main */
    memcpy(&SCD.sub_ctx, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &s_main_saved, sizeof(m68ki_cpu_core));

    return used;
}
