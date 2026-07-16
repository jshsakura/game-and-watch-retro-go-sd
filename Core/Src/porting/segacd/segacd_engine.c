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
uint32_t scd_dbg_chunks, scd_dbg_deliver4, scd_dbg_deliver2;

/* Boot-stall investigation (0716): frame counter the harness stamps every
 * iteration, so bus/engine-side trace points can log WHICH frame something
 * happened in without threading a frame argument through every call site. */
int scd_dbg_frame;

/* Ring buffers of (frame, PC) for the first few times each interrupt source
 * actually wins a delivery slot in segacd_run_sub's chunk loop — PC is the
 * SUB's PC (m68k == sub context here), captured BEFORE the chunk's m68k_run,
 * i.e. where the sub was parked when the interrupt was asserted to it. */
#define SCD_DBG_DELIVER_LOG_N 8
uint32_t scd_dbg_deliver2_frame[SCD_DBG_DELIVER_LOG_N], scd_dbg_deliver2_pc[SCD_DBG_DELIVER_LOG_N];
int scd_dbg_deliver2_n;
uint32_t scd_dbg_deliver4_frame[SCD_DBG_DELIVER_LOG_N], scd_dbg_deliver4_pc[SCD_DBG_DELIVER_LOG_N];
int scd_dbg_deliver4_n;
uint32_t scd_dbg_deliver5_total;
uint32_t scd_dbg_deliver5_frame[SCD_DBG_DELIVER_LOG_N], scd_dbg_deliver5_pc[SCD_DBG_DELIVER_LOG_N];
int scd_dbg_deliver5_n;
#endif

/* Saved MAIN context while the sub-CPU is running. Static, one level of
 * nesting only — the sub never re-enters the main. */
static m68ki_cpu_core s_main_saved;

/* The sub-CPU's own interrupt-ack callback (M68K_EMULATE_INT_ACK=OPT_ON calls
 * this on EVERY interrupt the sub actually takes — i.e. exactly when
 * m68ki_check_interrupts() finds CPU_INT_LEVEL > FLAG_INT_MASK and the
 * exception really fires, NOT merely when we've asked for one via
 * m68k_set_irq()). This must NOT be gwenesis's own m68k_irq_acked
 * (gwenesis_vdp_mem.c) — that function unconditionally reads/writes
 * gwenesis_vdp_status/hint_pending, which are MAIN's VDP globals, not
 * per-CPU state; the sub inheriting it via the segacd_reset() memcpy means
 * every sub interrupt ack (IFL2/CDD/CDC — all of them, several times a
 * frame) can spuriously steal MAIN's own pending-VBLANK flag or assert a
 * bogus level onto the sub's own core. Giving the sub this minimal callback
 * both stops that cross-contamination and is the correct place to clear our
 * one-shot pending flags: clearing them here (at ACTUAL ack time) instead of
 * at m68k_set_irq() PRESENT time (the old segacd_run_sub behavior) is what
 * fixes the race where a request presented while the sub's SR interrupt mask
 * still blocks it (e.g. immediately after entering a higher-priority
 * handler) got silently retracted next chunk before the sub ever took it —
 * see segacd_run_sub's delivery loop. */
static int segacd_sub_int_ack(int int_level)
{
    switch (int_level) {
    case 5: SCD.cdc_int_pending = 0; break;
    case 4: SCD.cdd_int_pending = 0; break;
    case 2: SCD.ga_ifl2         = 0; break;
    case 1: SCD.gfx_int_pending = 0; break;   /* GFX ASIC completion (level 1) */
    default: break;
    }
    /* Mirror default_int_ack_callback (m68kcpu.c): retract CPU_INT_LEVEL on
     * ack. Required, not optional — m68ki_exception_interrupt() raises
     * FLAG_INT_MASK to int_level but does NOT itself clear CPU_INT_LEVEL, so
     * without this the same still-asserted level re-triggers the instant RTE
     * drops the mask back down (the "interrupt storm" the level-triggered
     * design was rejected for — see segacd_run_sub's delivery-loop comment).
     * m68k.int_level is the public name of the internal CPU_INT_LEVEL field
     * (m68k.h); `m68k` here is the SUB's context (int_ack_callback only
     * fires from inside that CPU's own m68k_run). */
    m68k.int_level = 0;
    return M68K_INT_ACK_AUTOVECTOR;
}

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

    segacd_gfx_init();    /* build the graphics-ASIC lookup tables */
    segacd_reset();
}

void segacd_reset(void)
{
    if (SCD.prg_ram)  memset(SCD.prg_ram,  0, SEGACD_PRG_RAM_SIZE);
    if (SCD.word_ram) memset(SCD.word_ram, 0, SEGACD_WORD_RAM_SIZE);
    if (SCD.pcm_ram)  memset(SCD.pcm_ram,  0, SEGACD_PCM_RAM_SIZE);
    memset(SCD.s68k_regs, 0, sizeof(SCD.s68k_regs));

    /* Word-RAM ownership at reset: RET=1 (main owns), 2M mode, PRG bank 0 —
     * matches PicoDrive's PicoMemSetupCD (remap_prg_window(2,1)/
     * remap_word_ram(1), i.e. reg3 reset value 0x01). The dmna_ret_2m shadow
     * starts in sync with it. */
    SCD.s68k_regs[0x03] = 0x01;
    SCD.dmna_ret_2m     = 0x01;
    SCD.prg_bank  = 0;
    SCD.word_mode = 0;

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
    SCD.sub_ctx.int_ack_callback = segacd_sub_int_ack;
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

    /* Frame-pace the GFX ASIC completion: an op armed by a $FF8066 write last
     * frame becomes deliverable this frame (the animation is timed against the
     * ~1-frame render latency; delivering it in the same timeslice it was
     * triggered would let the animation loop run away at unbounded speed). */
    if (SCD.gfx_op_armed) { SCD.gfx_int_pending = 1; SCD.gfx_op_armed = 0; }

    /* Either pending interrupt must wake a spin-idled sub; only skip when
     * truly idle. (IEN gating happens below — an interrupt "pending" here
     * but not yet enabled still needs the sub to run so it CAN enable it.) */
    if (SCD.sub_idle && !SCD.cdd_int_pending && !SCD.ga_ifl2 &&
        !SCD.cdc_int_pending && !SCD.gfx_int_pending)
        return 0;
    SCD.sub_idle = 0;

    /* save main -> load sub */
    memcpy(&s_main_saved, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &SCD.sub_ctx, sizeof(m68ki_cpu_core));

    /* m68k_run() takes an ABSOLUTE cycle target, and the sub's cycle counter
     * persists in sub_ctx across timeslices. If we passed a fixed target the
     * sub would run one timeslice's worth of cycles ONCE and then every later
     * call would find m68k.cycles already past the target and do nothing — the
     * sub would freeze mid-instruction (it froze inside the BIOS self-checksum
     * sum loop at PC 0x2e0). Rebase to 0 each slice, exactly as the main frame
     * loop does with `m68k.cycles -= system_clock`. */
    m68k.cycles = 0;

    /* IFL2 delivery: TESTED AND REJECTED the "IFL2 is a held level, re-taken
     * after every RTE for as long as main leaves it asserted" hypothesis.
     * PicoDrive's OWN Musashi ack callback (pd_cd/sek.c SekIntAckMS68k ->
     * new_irq_level(2)) does `state_flags &= ~PCD_ST_S68K_IFL2` — i.e. real
     * hardware/PicoDrive auto-clears the pending IFL2 flag the instant the
     * sub ACKs the interrupt, same as gwenesis's Musashi
     * (default_int_ack_callback zeroing CPU_INT_LEVEL on take). So IFL2
     * really is one-shot-per-assertion, exactly like our ORIGINAL model —
     * that was not the bug. (Confirmed by testing the level-triggered
     * version first, per the debugging playbook: it desynced into an
     * interrupt storm — see commit history — because nothing on real
     * hardware ever re-fires it either.)
     *
     * The actual gap: the OLD code allowed AT MOST ONE level-4 (CDD) and ONE
     * level-2 (IFL2) delivery per segacd_run_sub() CALL (one video frame),
     * via a hardcoded "first pass, then one grace pass" structure. But
     * SCD.cdd_int_pending can legitimately re-arm itself MORE than once
     * per frame: the sub's own level-4 handler re-arms the CDD export
     * ("HOCK", $FF8037 bit2) as part of finishing one status packet, and
     * segacd_bus.c's case 0x37 fires that immediately when IEN4 is already
     * on (matching pd_cd/memory.c case 0x37) — this is a real, sub-driven,
     * same-frame re-trigger, not an artifact. A single frame's sub timeslice
     * can legitimately need several level-4 deliveries in a row (each one
     * one-shot, each individually gated on IEN4) before the doorbell
     * (IFL2, level 2, lower priority) ever gets its turn. Chunk the
     * timeslice so any number of one-shot deliveries can happen across the
     * SAME frame, each still consumed exactly once — level 4 wins the slot
     * over level 2 whenever both are live at once, matching real 68000 IPL
     * priority (CPU_INT_LEVEL is the single highest-asserted level, like the
     * physical IPL0-2 priority encoder). Both still gate on the sub's own
     * IEN mask ($FF8033), matching real hardware/pd_cd (memory.c:463-491
     * `case 0x33`/`case 0x37`). */
    #define SEGACD_IRQ_CHUNK_CYCLES 400u
    #define SEGACD_IRQ_CHUNK_GUARD  4096
    int guard = 0;
#ifdef SEGACD_GA_TRACE
    extern uint32_t scd_dbg_chunks, scd_dbg_deliver4, scd_dbg_deliver2;
    scd_dbg_chunks = scd_dbg_deliver4 = scd_dbg_deliver2 = 0;
#endif
    for (;;) {
        /* CDC (level 5) outranks CDD (level 4) outranks the IFL2 doorbell
         * (level 2) — matches real 68000 IPL priority (highest-asserted
         * level wins) and pd_cd's own vector assignment (CDC is the
         * highest-numbered CD interrupt source). Gated on IEN5 ($FF8033
         * bit5), same one-shot-per-assertion contract as level 4/2. */
        int want5 = SCD.cdc_int_pending && (SCD.s68k_regs[0x33] & 0x20);
        int want4 = !want5 && SCD.cdd_int_pending && (SCD.s68k_regs[0x33] & 0x10);
        int want2 = !want5 && !want4 && SCD.ga_ifl2 && (SCD.s68k_regs[0x33] & 0x04);
        /* GFX ASIC completion, level 1 — lowest priority, gated on IEN1
         * ($FF8033 bit1). One-shot per assertion like the others; cleared at
         * ACK in segacd_sub_int_ack case 1. */
        int want1 = !want5 && !want4 && !want2 &&
                    SCD.gfx_int_pending && (SCD.s68k_regs[0x33] & 0x02);
#ifdef SEGACD_GA_TRACE
        scd_dbg_chunks++;
        if (want5) {
            scd_dbg_deliver5_total++;
            if (scd_dbg_deliver5_n < SCD_DBG_DELIVER_LOG_N) {
                scd_dbg_deliver5_frame[scd_dbg_deliver5_n] = (uint32_t)scd_dbg_frame;
                scd_dbg_deliver5_pc[scd_dbg_deliver5_n] = m68k.pc;
                scd_dbg_deliver5_n++;
            }
        }
        if (want4) {
            scd_dbg_deliver4++;
            if (scd_dbg_deliver4_n < SCD_DBG_DELIVER_LOG_N) {
                scd_dbg_deliver4_frame[scd_dbg_deliver4_n] = (uint32_t)scd_dbg_frame;
                scd_dbg_deliver4_pc[scd_dbg_deliver4_n] = m68k.pc;
                scd_dbg_deliver4_n++;
            }
        }
        if (want2) {
            scd_dbg_deliver2++;
            if (scd_dbg_deliver2_n < SCD_DBG_DELIVER_LOG_N) {
                scd_dbg_deliver2_frame[scd_dbg_deliver2_n] = (uint32_t)scd_dbg_frame;
                scd_dbg_deliver2_pc[scd_dbg_deliver2_n] = m68k.pc;
                scd_dbg_deliver2_n++;
            }
        }
#endif

        /* Re-present the level every chunk, including 0 — a stale
         * CPU_INT_LEVEL left over from a source that's since gone quiet
         * (consumed via ack, or IEN turned off) must be retracted explicitly,
         * or the CPU could still take an interrupt that is no longer
         * logically pending/enabled.
         *
         * Do NOT clear the pending flags here. The old code cleared
         * cdc/cdd_int_pending / ga_ifl2 the instant a source WON a chunk's
         * priority arbitration (i.e. the instant we asked for it), not when
         * the sub's core actually TOOK it. That's a race: if the sub's SR
         * interrupt mask still blocks the level this chunk (e.g. it just
         * entered a higher-priority handler and hasn't RTE'd back down yet —
         * observed at the boot stall: level-2 "won" a chunk while the sub's
         * PC was sitting at the level-4 handler's own entry, mask still 4),
         * m68ki_check_interrupts() silently does NOT take it, yet we'd
         * already cleared the flag — so the NEXT chunk finds want2 false and
         * re-presents 0, retracting the request before the sub ever got a
         * chance to. The doorbell pulse was lost with no trace beyond "sub
         * never left the wait loop". Fix: keep the flag (and keep
         * re-presenting the level) until segacd_sub_int_ack ACTUALLY fires —
         * that only happens when m68ki_exception_interrupt truly runs, i.e.
         * the sub really took it. Still one-shot (cleared exactly once, at
         * ack), just correctly timed instead of optimistically early. */
        m68k_set_irq((unsigned int)(want5 ? 5 : (want4 ? 4 : (want2 ? 2 : (want1 ? 1 : 0)))));

        if (!want5 && !want4 && !want2 && !want1) {
            /* Nothing to chunk for: run the rest of the slice in one go
             * (the common case — no cost over the old unchunked path). */
            m68k_run((unsigned int)cycle_target);
            break;
        }

        unsigned int next = m68k.cycles + SEGACD_IRQ_CHUNK_CYCLES;
        if (next > (unsigned int)cycle_target) next = (unsigned int)cycle_target;
        m68k_run(next);

        if (m68k.cycles >= (unsigned int)cycle_target) break;
        if (++guard >= SEGACD_IRQ_CHUNK_GUARD) {
            /* Safety valve: something is re-arming want2/want4 every single
             * chunk for an implausibly long time. Finish the slice
             * unchunked rather than spin here forever. */
            m68k_run((unsigned int)cycle_target);
            break;
        }
    }
    #undef SEGACD_IRQ_CHUNK_CYCLES
    #undef SEGACD_IRQ_CHUNK_GUARD

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
