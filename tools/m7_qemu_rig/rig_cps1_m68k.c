/*
 * How much of a 60fps frame does the CPS-1 68000 cost, on THIS device's ISA?
 *
 * WHY THIS RIG EXISTS, AND WHY ITS NUMBER TRANSFERS WHERE THE OLD ONE DID NOT
 * -------------------------------------------------------------------------
 * rig_cps1.c measures a whole synthetic frame: a nine-opcode toy CPU running
 * a made-up test program plus a made-up scene. Its "18.6 ms" says nothing
 * about Tenchi wo Kurau II, because none of what it executed is what that
 * game executes.
 *
 * This rig measures ONE ratio instead: ARM instructions retired per emulated
 * 68000 CYCLE, for Musashi. That ratio transfers to a real game, because of
 * a hardware fact that bounds every CPS-1 title ever made:
 *
 *     CPS-1's 68000 runs at 10 MHz. At 60 fps that is
 *     10,000,000 / 60 = 166,667 68000 cycles per frame, MAXIMUM.
 *
 * A real game cannot exceed that -- the real board doesn't either. So
 *     instructions/frame(CPU) <= 166,667 * (instructions per 68000 cycle)
 * is an upper bound on the CPU half of the frame for ANY CPS-1 game,
 * measured rather than guessed. Compare it against the device budget
 * (340 MHz / 60 = 5,666,666 instructions/frame) and the CPU question is
 * answered in the only terms that matter.
 *
 * WHAT IT STILL DOES NOT ANSWER: the graphics half. Sprite/tile fill cost
 * depends on what the game actually draws, and no synthetic scene predicts
 * that. This rig deliberately measures only the part that CAN be bounded
 * from hardware facts. Nor does QEMU model cache misses or flash wait
 * states (mps2_an500.ld's own header says so) -- on the real part, Musashi's
 * 256 KB jump table living in XIP flash will cost more than this shows.
 * Treat the result as a floor for the CPU, not a promise.
 *
 * The workload is a deliberately ORDINARY inner loop -- load from RAM,
 * arithmetic, store back, compare, conditional branch -- i.e. the shape of
 * real game logic rather than a microbenchmark of one instruction. It is
 * still synthetic; the honest claim is "typical mix", not "this game".
 */
#include <stdio.h>
#include <stdint.h>

#include "cps1_m68k.h"

#ifndef RIG_CYCLES
#define RIG_CYCLES 2000000u  /* in MUL-scaled units, see CPS1_M68K_MUL */
#endif

/*
 * THE TRAP THIS RIG WALKED INTO ONCE, DO NOT WALK INTO IT AGAIN.
 * m68kcpu.c does `#define MUL (7)` and m68ki_cycles.h is PRE-MULTIPLIED by
 * it (its first rows read literally "8*7, 8*7, ... 16*7, ... 18*7"), because
 * Genesis clocks its 68000 at masterclock/7. So m68k.cycles counts MASTER
 * cycles, NOT 68000 cycles -- taking it at face value overstates the core's
 * speed by exactly 7x, which is how a first run of this rig produced an
 * impossible "0.891 ARM instructions per 68000 cycle". Divide it out.
 */
#define CPS1_M68K_MUL 7u

/* CPS-1 hardware facts (docs/CPS1_MAME_ALIGNMENT.md section 4). */
#define CPS1_M68K_CLOCK_HZ     10000000ull
#define CPS1_CYCLES_PER_FRAME  ((uint32_t)(CPS1_M68K_CLOCK_HZ / 60ull)) /* 166,666 */

/* Device budget, same assumption every other cps1 rig uses. */
#define DEVICE_CLOCK_HZ        340000000ull
#define DEVICE_BUDGET_INSN     ((uint64_t)(DEVICE_CLOCK_HZ / 60ull))    /* 5,666,666 */

void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

#define ROM_WORDS  (0x10000u / 2u)
#define WRAM_BYTES 0x10000u

static uint16_t s_rom[ROM_WORDS];   /* native 16-bit stores => byte-swapped layout */
static uint8_t  s_wram[WRAM_BYTES];

/*
 * 0x0000 .long $00FF1000    SSP
 * 0x0004 .long $00000008    PC
 * 0x0008 LEA    $00FF0000,A0
 * 0x000E MOVEQ  #0,D0
 * 0x0010 MOVE.W (A0),D0     <- loop top: RAM read
 * 0x0012 ADDQ.W #1,D0
 * 0x0014 MOVE.W D0,(A0)                  RAM write
 * 0x0016 CMPI.W #$1000,D0
 * 0x001A BNE.S  -> 0x0010
 * 0x001C MOVEQ  #0,D0
 * 0x001E BRA.S  -> 0x0010
 */
static const uint16_t k_prog[] = {
    /* 0x0000 */ 0x00FF, 0x1000,
    /* 0x0004 */ 0x0000, 0x0008,
    /* 0x0008 */ 0x41F9, 0x00FF, 0x0000,
    /* 0x000E */ 0x7000,
    /* 0x0010 */ 0x3010,
    /* 0x0012 */ 0x5240,
    /* 0x0014 */ 0x3080,
    /* 0x0016 */ 0x0C40, 0x1000,
    /* 0x001A */ 0x66F4,
    /* 0x001C */ 0x7000,
    /* 0x001E */ 0x60F0,
};

static uint16_t io_read16(uint32_t addr)  { (void)addr; return 0; }
static void     io_write16(uint32_t addr, uint16_t val) { (void)addr; (void)val; }

int main(void)
{
    rig_timer_init();
    uint32_t cal_ticks = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));

    for (unsigned i = 0; i < sizeof(k_prog) / sizeof(k_prog[0]); i++)
        s_rom[i] = k_prog[i];

    const cps1_m68k_io_t io = { io_read16, io_write16 };
    cps1_m68k_init((const uint8_t *)s_rom, sizeof(s_rom), s_wram, &io);
    cps1_m68k_reset();

    printf("[cps1-m68k-qemu] Musashi on ARMv7-M; cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

    uint32_t t0 = rig_timer_now();
    uint32_t consumed = cps1_m68k_run(RIG_CYCLES);
    uint32_t t1 = rig_timer_now();

    uint64_t insns = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000ull;
    /* consumed is in MUL-scaled master cycles -- convert to real 68000 cycles. */
    uint32_t consumed_68k = consumed / CPS1_M68K_MUL;
    if (consumed_68k == 0) consumed_68k = 1;

    /* x1000 fixed point: this ratio is the whole point of the rig. */
    uint64_t insn_per_cyc_x1000 = insns * 1000ull / consumed_68k;
    uint64_t cpu_insn_per_frame = insn_per_cyc_x1000 * CPS1_CYCLES_PER_FRAME / 1000ull;
    double cpu_ms = (double)cpu_insn_per_frame * 1000.0 / (double)DEVICE_CLOCK_HZ;
    uint64_t pct_x10 = cpu_insn_per_frame * 1000ull / DEVICE_BUDGET_INSN;

    printf("[cps1-m68k-qemu] ran %lu master cycles = %lu REAL 68000 cycles (MUL=%u) in %lu ARM insns\n",
           (unsigned long)consumed, (unsigned long)consumed_68k,
           CPS1_M68K_MUL, (unsigned long)insns);
    printf("[cps1-m68k-qemu] ratio = %lu.%03lu ARM insn per 68000 cycle\n",
           (unsigned long)(insn_per_cyc_x1000 / 1000), (unsigned long)(insn_per_cyc_x1000 % 1000));
    printf("[cps1-m68k-qemu] CPS-1 68000 @10MHz = %lu cycles/frame (hardware max)\n",
           (unsigned long)CPS1_CYCLES_PER_FRAME);
    printf("[cps1-m68k-qemu] => CPU UPPER BOUND: %lu insn/frame = %.3f ms @340MHz "
           "= %lu.%lu%% of the 60fps budget (%llu insn)\n",
           (unsigned long)cpu_insn_per_frame, cpu_ms,
           (unsigned long)(pct_x10 / 10), (unsigned long)(pct_x10 % 10),
           (unsigned long long)DEVICE_BUDGET_INSN);
    printf("[cps1-m68k-qemu] leaves %.3f ms of the 16.667 ms frame for graphics+sound\n",
           16.6667 - cpu_ms);
    printf("[cps1-m68k-qemu] NOTE: floor, not promise -- QEMU models no cache/flash-wait, "
           "and the 256KB jump table will be XIP on the real part.\n");
    return 0;
}
