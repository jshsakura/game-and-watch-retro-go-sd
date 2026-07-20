/* CPS-1 on the M7 QEMU rig -- real ARMv7-M instruction stream.
 *
 * Compiles the SAME Core/Src/porting/cps1/cps1_core.c stub the linux/cps1
 * harness uses (docs/CPS1_FEASIBILITY.md Phase 2), so once real 68000/Z80/
 * PPU/sound code replaces the stub, this rig measures its instructions/frame
 * on a real ARMv7-M stream the same way rig_vb.c does for Virtual Boy.
 * Reuses rig_runtime.c and mps2_an500.ld verbatim -- both are core-agnostic
 * (docs/HARNESSES.md: "copy rig_vb.c's shape to put any other core on the
 * same scale"). Until a real core lands, this only proves the plumbing:
 * calibrated timer, frame loop, per-window instruction ledger, RUNHASH.
 */
#include <stdio.h>
#include <stdint.h>

#include "cps1_core.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 600
#endif
#define RIG_WINDOW 100

void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

int main(void)
{
    rig_timer_init();
    /* Calibrate ticks -> instructions: the loop body is exactly 3 insns. */
    uint32_t cal_ticks = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
    printf("[cps1-qemu] STUB rig -- no real core yet, see docs/CPS1_FEASIBILITY.md\n");
    printf("[cps1-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

    cps1_core_reset(CPS1_ENGINE_INTERPRETER);
    printf("[cps1-qemu] cpu test program halted -- initial state hash=%08x illegal=%u\n",
           (unsigned)cps1_core_cpu_state_hash(CPS1_ENGINE_INTERPRETER),
           (unsigned)cps1_core_cpu_illegal_count(CPS1_ENGINE_INTERPRETER));

    uint32_t run_hash = 2166136261u;
    uint64_t win_ticks = 0, tot_ticks = 0;

    for (int frame = 0; frame < RIG_FRAMES; frame++) {
        uint32_t t0 = rig_timer_now();
        cps1_core_run_frame(CPS1_ENGINE_INTERPRETER);
        uint32_t t1 = rig_timer_now();
        win_ticks += (uint32_t)(t1 - t0);

        uint32_t h = cps1_core_checksum(CPS1_ENGINE_INTERPRETER);
        run_hash = (run_hash ^ h) * 16777619u;

        if ((frame + 1) % RIG_WINDOW == 0) {
            uint64_t emu_i = win_ticks * ipt_x1000 / 1000 / RIG_WINDOW;
            printf("w%05d emu=%lu insn/frame fb=%08x\n",
                   frame + 1, (unsigned long)emu_i, (unsigned)h);
            tot_ticks += win_ticks;
            win_ticks = 0;
        }
    }

    uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
    if (frames == 0) frames = 1;
    printf("[cps1-qemu] done %d frames RUNHASH=%08x avg emu=%lu insn/frame\n",
           RIG_FRAMES, (unsigned)run_hash,
           (unsigned long)(tot_ticks * ipt_x1000 / 1000 / frames));
    printf("[cps1-qemu] cpu final state hash=%08x illegal=%u\n",
           (unsigned)cps1_core_cpu_state_hash(CPS1_ENGINE_INTERPRETER),
           (unsigned)cps1_core_cpu_illegal_count(CPS1_ENGINE_INTERPRETER));
    return 0;
}
