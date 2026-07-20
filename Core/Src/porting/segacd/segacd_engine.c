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

/* Per-ISR chunk attribution for the b9 ISR-budget subdivision (always-on,
 * 32 bytes BSS, 1 inc/chunk — negligible). Bucket 0 = foreground; 1/2/4/5 =
 * GFX/IFL2/CDD/CDC. Multiply by SEGACD_IRQ_CHUNK_CYCLES (400) for emulated
 * cycles. Reset externally before a benchmark window. */
uint32_t scd_sub_isr_chunks[8];

/* General-purpose idle-skip controls (harness-side optimization preview).
 * When scd_sub_idle_skip is set, the sub-CPU's chunk loop will fast-forward
 * through tight spin loops instead of executing the TST/BNE literally.
 * scd_sub_idle_hits counts how many slices were skipped this way. Both
 * default to 0 — no behavior change unless explicitly enabled. */
int scd_sub_idle_skip;
uint32_t scd_sub_idle_hits;

/* Detect a tight spin loop at the given PC. Returns 1 if the instructions
 * at PC form a backward-branching loop of <= 6 bytes — the canonical 68K
 * wait-for-interrupt idiom. Covers every pattern observed in the SegaCD
 * histogram (main $fe26 WaitVSync, sub $36a9 semaphore, sub $6194 reg-poll,
 * main BTST poll, and the 32X-style BRA-self):
 *
 *   <test/poll op> + Bcc.S *-N   (op is 2, 4, or 6 bytes)
 *   BRA.S self (0x60FE)
 *
 * The Bcc.S displacement is the signal: a negative branch landing at or
 * before the loop entry PC. Uses m68k_read_disassembler_16 (already linked
 * for the z80inst disassembler path) — not REG_IR, which isn't set yet at
 * the run-site call. Game/BIOS-agnostic: matches the OPCODE pattern, not
 * specific PCs. */
int scd_m68k_is_spin(unsigned int pc)
{
    unsigned int op = m68k_read_disassembler_16(pc);

    /* BRA.S self — tightest possible spin (0x60FE) */
    if (op == 0x60FE) return 1;

    /* Scan for a Bcc.S at +2, +4, or +6 (2/4/6-byte op1) that branches
     * backward to <= pc. Bcc.S high nibble 0x6, displacement is low byte
     * sign-extended. Target = bcc_addr + 2 + disp (68K PC-relative math). */
    unsigned int offs[3] = {2, 4, 6};
    for (int i = 0; i < 3; i++) {
        unsigned int bcc = m68k_read_disassembler_16(pc + offs[i]);
        if ((bcc & 0xF000) != 0x6000) continue;
        int disp = (int)(int8_t)(bcc & 0xFF);
        if (disp >= 0) continue;                 /* forward branch = not a spin */
        unsigned int target = pc + offs[i] + 2 + (unsigned int)disp;
        if (target <= pc) return 1;              /* loops back to entry */
    }
    return 0;
}

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

/* Rebuild Word-RAM ownership in both CPUs' memory maps — see segacd.h for the
 * contract. Mirrors PicoDrive's remap_word_ram() (pd_cd/memory.c):
 *
 *   2M mode: a single 256KB block. Both CPUs' windows point at the same
 *   SCD.word_ram regardless of the RET/DMNA ownership bit — real hardware
 *   lets the non-owning side's access through too (data-wise identical; it
 *   just stalls that CPU on the bus as an arbitration side effect we don't
 *   model, since well-behaved software honors DMNA/RET and doesn't rely on
 *   the stall). This matches the ORIGINAL static mapping this port shipped
 *   with, so 2M-mode behavior is unchanged.
 *
 *   1M mode: two independent 128KB banks (the two halves of the same
 *   256KB SCD.word_ram buffer — no separate cell-interleaved storage, see
 *   below). Bit0 of reg3 ("b0") selects which half MAIN sees at
 *   $200000-$21FFFF; SUB always gets the OTHER half at $0C0000-$0DFFFF.
 *   This is the actual hardware ownership split — each CPU has its own
 *   private bank with no aliasing, unlike the single-buffer placeholder
 *   this port shipped with (see the removed comments at the old static
 *   map[0x0C]/map[0x20+p] assignments this replaces).
 *
 * NOT implemented: the "cell arrange"/"decode format" dot-transform views
 * real hardware exposes at $220000-$23FFFF (main) / $080000-$0BFFFF (sub)
 * in 1M mode for the GFX ASIC's interleaved addressing (pd_cd/memory.c,
 * the m68k_cell and s68k_dec handler families). No boot-path or currently-
 * ported GFX ASIC code depends on that byte layout; those windows are
 * pointed at the OTHER bank in plain linear form (better than aliasing
 * onto the CPU's own bank, not byte-accurate cell-interleave) as a
 * documented approximation. TODO: real cell/dot-decode transform if a
 * title's renderer needs it. */
void segacd_word_ram_remap(int called_from_sub)
{
    cpu_memory_map *main_map = called_from_sub ? s_main_saved.memory_map : m68k.memory_map;
    cpu_memory_map *sub_map  = called_from_sub ? m68k.memory_map : SCD.sub_ctx.memory_map;
    uint8_t r3 = SCD.s68k_regs[0x03];
    int p;

    if (!(r3 & 0x04)) {
        /* 2M mode */
        for (p = 0; p < 4; p++) {
            main_map[0x20 + p].base = SCD.word_ram + (unsigned)p * 0x10000u;
            sub_map [0x08 + p].base = SCD.word_ram + (unsigned)p * 0x10000u;
        }
        /* $0C0000-$0DFFFF is only meaningful in 1M mode; point it at
         * word_ram start so a stray 2M-mode access doesn't fault. */
        sub_map[0x0C].base = SCD.word_ram;
        sub_map[0x0D].base = SCD.word_ram + 0x10000u;
    } else {
        /* 1M mode: bank[b0] -> MAIN, bank[b0^1] -> SUB. */
        int b0 = r3 & 1;
        unsigned char *main_bank = SCD.word_ram + (unsigned)b0 * 0x20000u;
        unsigned char *sub_bank  = SCD.word_ram + (unsigned)(b0 ^ 1) * 0x20000u;

        main_map[0x20].base = main_bank;
        main_map[0x21].base = main_bank + 0x10000u;
        sub_map [0x0C].base = sub_bank;
        sub_map [0x0D].base = sub_bank + 0x10000u;

        /* Approximated cell-arrange views (see function comment). */
        main_map[0x22].base = sub_bank;
        main_map[0x23].base = sub_bank + 0x10000u;
        for (p = 0; p < 4; p++)
            sub_map[0x08 + p].base = SCD.word_ram + (unsigned)p * 0x10000u;
    }
    SCD.word_owner = (uint8_t)(r3 & 1);
}

/* DEBUG: dump interrupt state at segacd_run_sub entry */
void segacd_dump_int_state(int frame)
{
    /* Sub SR interrupt mask: bits 10-8 of SR */
    uint16_t sub_sr = SCD.sub_ctx.int_level << 8; /* FLAG_INT_MASK field */
    extern uint32_t scd_dbg_dump_seen;
    if (!scd_dbg_dump_seen) scd_dbg_dump_seen = 0;
    printf("[irqst] f%d entry: cdd_pend=%d cdc_pend=%d ga_ifl2=%d ien=%02x reg37=%02x subPC=%06x sub_intmask=%d\n",
           frame,
           SCD.cdd_int_pending, SCD.cdc_int_pending, SCD.ga_ifl2,
           SCD.s68k_regs[0x33], SCD.s68k_regs[0x37],
           SCD.sub_ctx.pc, (sub_sr >> 8) & 7);
}

/* Run the sub-68K up to a cycle target, with its context swapped in.
 * Returns cycles actually consumed. No-op while the sub is held or idle. */
int segacd_run_sub(int cycle_target)
{
    if (!SCD.sub_running)
        return 0;
#ifdef SEGACD_GA_TRACE
    /* At entry, m68k is still the MAIN context (the caller just ran it). Does
     * the MAIN ever reach its stamp-draw routines (0x619e fills Word-RAM
     * $220000+, callers 0x5f22..0x6de8)? GPGX runs these every frame to build
     * the logo the GFX ASIC renders from; if ours never does, the stamp map
     * stays empty and the render is blank. */
    { extern uint32_t scd_dbg_mainstamp_hits;
      if (m68k.pc >= 0x5f00 && m68k.pc < 0x6e00) scd_dbg_mainstamp_hits++; }
#endif

    /* Frame-pace the GFX ASIC completion: an op armed by a $FF8066 write last
     * frame becomes deliverable this frame (the animation is timed against the
     * ~1-frame render latency; delivering it in the same timeslice it was
     * triggered would let the animation loop run away at unbounded speed). */
    if (SCD.gfx_op_armed) { SCD.gfx_int_pending = 1; SCD.gfx_op_armed = 0; }

    /* Either pending interrupt must wake a spin-idled sub; only skip when
     * truly idle. (IEN gating happens below — an interrupt "pending" here
     * but not yet enabled still needs the sub to run so it CAN enable it.)
     * Clear sub_idle HERE: the flag was set by poll detection during a
     * previous slice. If we reach this point (didn't return above), the sub
     * will actually run, so clear the idle flag — the sub will re-set it
     * via poll detection if it re-enters a spin loop.
     *
     * LANDMINE: this list is only 4 of the sub's interrupt levels (1/2/4/5).
     * Level 6 (subcode) is not modeled anywhere in this engine yet, and the
     * mode-8 boot-crossing investigation's leading suspect for what clears
     * $36a9 is exactly a level-6 delivery we haven't wired
     * (session-handoff-0716-segacd.md). It's harmless today because level 6
     * doesn't exist here to be missed. The day someone adds a
     * subcode_int_pending flag for that fix, it MUST be added to this
     * condition too — otherwise this idle-skip gate silently swallows the
     * fix by returning 0 forever without ever giving the sub a chance to
     * see the new interrupt. */
    if (SCD.sub_idle && !SCD.cdd_int_pending && !SCD.ga_ifl2 &&
        !SCD.cdc_int_pending && !SCD.gfx_int_pending)
        return 0;
    SCD.sub_idle = 0;

    /* DEBUG: dump interrupt state at entry for critical frames */
    {
        extern int scd_dbg_frame;
        if ((scd_dbg_frame >= 735 && scd_dbg_frame <= 760) ||
            (scd_dbg_frame >= 83 && scd_dbg_frame <= 90)) {
            extern void segacd_dump_int_state(int frame);
            segacd_dump_int_state(scd_dbg_frame);
        }
    }

    /* save main -> load sub */
    memcpy(&s_main_saved, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &SCD.sub_ctx, sizeof(m68ki_cpu_core));
#ifdef HOOK_CPU
    /* histogram attribution: sub-68K context is now in the shared `m68k`
     * global, so per-insn samples taken by the HOOK_CPU path from here on
     * must be attributed to SUB, not MAIN. */
    extern int g_scd_hist_issub;
    g_scd_hist_issub = 1;
#endif
#if defined(HOOK_CPU) && defined(SCD_CACHE)
    /* cache-only build: enable hook only during sub execution to avoid
     * ~10M wasted calls during main 68K run */
    extern void scd_cache_hook_enable(int);
    scd_cache_hook_enable(1);
#endif

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
    /* Per-ISR chunk histogram (b9 ISR-budget subdivision). Each chunk is
     * SEGACD_IRQ_CHUNK_CYCLES (400) cycles, so chunks_per_level * 400 is the
     * emulated-cycle cost of that level. Bucket 0 = foreground (no IRQ won
     * the priority arbitration that chunk). Always-on: tiny overhead (1 inc
     * per chunk, max GUARD chunks/slice), useful in both bench and hist builds,
     * firmware-safe. */
    extern uint32_t scd_sub_isr_chunks[8];
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
        /* per-ISR chunk attribution (bucket 0 = foreground/no-IRQ chunk) */
        scd_sub_isr_chunks[want5 ? 5 : (want4 ? 4 : (want2 ? 2 : (want1 ? 1 : 0)))]++;
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
             * (the common case — no cost over the old unchunked path).
             * Poll detection (sub_ff_read8) handles idle-skip at the
             * memory-access level — no opcode-pattern matching needed. */
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
    /* Accumulate absolute sub cycles for poll-timing (POLL_CYCLES check in
     * sub_ff_read8 needs an absolute counter that survives the per-slice
     * rebase at line 278). m68k.cycles is rebased to 0 each slice, so
     * adding it here gives the true total. */
    SCD.sub_cycle_accum += (uint32_t)used;
#ifdef SEGACD_GA_TRACE
    /* Track the furthest PC the sub reaches past the self-checksum (which PASSES:
     * sum 0x200..0x5800 == 0xe9bb). */
    extern uint32_t scd_dbg_maxpc;
    if (m68k.pc > scd_dbg_maxpc && m68k.pc < 0x6000) scd_dbg_maxpc = m68k.pc;
    /* Boot-logo state machine: does the sub ever enter STATE 8 (0x7136, the
     * stamp-graphics loader) and its stamp-draw path (0x74cc/0x7586/0x75ca)? */
    extern uint32_t scd_dbg_state8_hits, scd_dbg_stampwr_hits;
    if (m68k.pc >= 0x7136 && m68k.pc < 0x7600) scd_dbg_state8_hits++;
    if ((m68k.pc >= 0x74b6 && m68k.pc < 0x74e0) ||
        (m68k.pc >= 0x7586 && m68k.pc < 0x75e0)) scd_dbg_stampwr_hits++;
#endif

    /* save sub -> restore main */
    memcpy(&SCD.sub_ctx, &m68k, sizeof(m68ki_cpu_core));
    memcpy(&m68k, &s_main_saved, sizeof(m68ki_cpu_core));
#ifdef HOOK_CPU
    /* histogram attribution: sub no longer loaded into `m68k`. */
    extern int g_scd_hist_issub;
    g_scd_hist_issub = 0;
#endif
#if defined(HOOK_CPU) && defined(SCD_CACHE)
    extern void scd_cache_hook_enable(int);
    scd_cache_hook_enable(0);
#endif

    return used;
}

#ifdef SEGACD_GA_TRACE
uint32_t scd_dbg_state8_hits, scd_dbg_stampwr_hits;
#endif

#ifdef SEGACD_GA_TRACE
uint32_t scd_dbg_mainstamp_hits;
#endif
