/* CPS-1 on the M7 QEMU rig -- real ARMv7-M instruction stream.
 *
 * Compiles the SAME Core/Src/porting/cps1/cps1_core.c the linux/cps1
 * harness uses -- 68000 interpreter+bus, 3-layer BG+compositor, sound HLE,
 * all against synthetic ROM/scene data -- so this rig measures the
 * integrated pipeline's instructions/frame on a real ARMv7-M stream the
 * same way rig_vb.c does for Virtual Boy. Reuses rig_runtime.c and
 * mps2_an500.ld verbatim -- both are core-agnostic (docs/HARNESSES.md:
 * "copy rig_vb.c's shape to put any other core on the same scale").
 *
 * Measures THREE paths, because they answer different questions:
 *   - cps1_core_run_frame(): CPU + all 3 BG layers + sprites + the host
 *     compositor stand-in for LTDC. This is what a host WITHOUT real LTDC
 *     hardware has to pay -- not what the device pays.
 *   - cps1_core_run_frame_device_cost(): CPU + SCROLL3 + sprites only,
 *     alone. Phases 7-10's number -- SCROLL1/SCROLL2 hadn't been given a
 *     real home yet, so they were left out entirely. Kept here for
 *     historical comparison, NOT the budget number any more (see next).
 *   - "ltdc" (run_frame_ltdc_total): device_cost PLUS
 *     cps1_core_render_ltdc_bottom() (SCROLL1+SCROLL2 combined, Phase 12).
 *     This is the COMPLETE real per-frame cost under the actual LTDC
 *     dual-layer architecture -- device_cost's own buffer becomes LTDC
 *     layer 1, the bottom buffer becomes layer 0, and LTDC's hardware
 *     blends them for free; both layers' CONTENT is still real CPU work.
 *     THIS is the number to check against the 60fps budget now.
 *
 * What this rig can and can't tell you: QEMU's -icount models a real
 * ARMv7-M *instruction stream* (real Thumb-2 encoding, real hard-float
 * ABI), so instructions/frame here is a real number on the device's own
 * ISA -- not an x86 proxy. It does NOT model caches or flash wait states,
 * so an instruction count under budget is necessary, not sufficient, for
 * 60fps -- the device DWT/frame ledger is still the final judge (see
 * CLAUDE.md's "Testing a core the way the device runs it" and every prior
 * initiative in this repo that learned this the hard way).
 */
#include <stdio.h>
#include <stdint.h>

#include "cps1_core.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 300
#endif
#define RIG_WINDOW 50

/* Device clock assumption (STM32H7B0 PLL, matches the 340MHz figure in
 * docs/CPS1_SENIOR_TRICKS_ANALYSIS.md section 4's "340MHz/60 = 5.6M cycles
 * budget"). insn count is treated as a cycle-count proxy (1 insn ~= 1
 * cycle) -- the same approximation this repo's other QEMU rigs use, and
 * the same one that does NOT capture cache misses or flash wait states. */
#define CPS1_DEVICE_CLOCK_HZ   340000000ull
#define CPS1_FRAME_BUDGET_MS   16.6667
#define CPS1_FRAME_BUDGET_CYC  ((uint64_t)(CPS1_DEVICE_CLOCK_HZ / 60ull)) /* ~5.667M */

void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

typedef void (*frame_fn_t)(cps1_engine_kind_t);

/* Phase 12: the complete real per-frame cost -- device_cost's own buffer
 * becomes LTDC layer 1 (top), this ALSO renders SCROLL1+SCROLL2 into the
 * buffer that becomes layer 0 (bottom). See file header. */
static void run_frame_ltdc_total(cps1_engine_kind_t engine)
{
    cps1_core_run_frame_device_cost(engine);
    cps1_core_render_ltdc_bottom();
}

static uint64_t measure_path_engine(const char *label, frame_fn_t fn, uint32_t ipt_x1000,
                                     cps1_engine_kind_t engine);

static uint64_t measure_path(const char *label, frame_fn_t fn, uint32_t ipt_x1000)
{
    return measure_path_engine(label, fn, ipt_x1000, CPS1_ENGINE_INTERPRETER);
}

/* Optimization phase: same workload, CPS1_ENGINE_RECOMPILER instead of
 * CPS1_ENGINE_INTERPRETER -- isolates dispatch-mechanism cost (fetch/
 * decode/execute per instruction vs. pre-translated goto-threaded C) for
 * the IDENTICAL instruction stream (s_cpu_test_program, cps1_core.c),
 * since both engines already execute that program bit-identically (the
 * diff harness's whole reason to exist). "Basic-block coverage" isn't
 * expandable in any way that changes THIS number: the recompiler already
 * translates 100% of this fixed test program's opcodes (that's WHY
 * interpreter and recompiler agree bit-for-bit) -- there is no
 * uncovered opcode here to fall back to the interpreter for. A real
 * ROM's much larger opcode footprint is a different, larger undertaking
 * this synthetic harness can't exercise without one. */
static uint64_t measure_path_engine(const char *label, frame_fn_t fn, uint32_t ipt_x1000,
                                     cps1_engine_kind_t engine)
{
    cps1_core_reset(engine);

    uint32_t run_hash = 2166136261u;
    uint64_t win_ticks = 0, tot_ticks = 0;

    for (int frame = 0; frame < RIG_FRAMES; frame++) {
        uint32_t t0 = rig_timer_now();
        fn(engine);
        uint32_t t1 = rig_timer_now();
        win_ticks += (uint32_t)(t1 - t0);

        uint32_t h = cps1_core_checksum(engine);
        run_hash = (run_hash ^ h) * 16777619u;

        if ((frame + 1) % RIG_WINDOW == 0) {
            uint64_t emu_i = win_ticks * ipt_x1000 / 1000 / RIG_WINDOW;
            printf("[%s] w%05d emu=%lu insn/frame fb=%08x\n",
                   label, frame + 1, (unsigned long)emu_i, (unsigned)h);
            tot_ticks += win_ticks;
            win_ticks = 0;
        }
    }

    uint64_t frames = (uint64_t)((RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW);
    if (frames == 0) frames = 1;
    uint64_t avg_insn = tot_ticks * ipt_x1000 / 1000 / frames;

    double ms = (double)avg_insn * 1000.0 / (double)CPS1_DEVICE_CLOCK_HZ;
    printf("[%s] done %d frames RUNHASH=%08x avg=%lu insn/frame ~= %.4f ms @ %lluMHz "
           "(budget %.4f ms = %llu insn)\n",
           label, RIG_FRAMES, (unsigned)run_hash, (unsigned long)avg_insn, ms,
           (unsigned long long)(CPS1_DEVICE_CLOCK_HZ / 1000000ull),
           CPS1_FRAME_BUDGET_MS, (unsigned long long)CPS1_FRAME_BUDGET_CYC);
    printf("[%s] verdict: %s\n", label,
           (avg_insn <= CPS1_FRAME_BUDGET_CYC) ? "UNDER 60fps insn budget" : "OVER 60fps insn budget");

    return avg_insn;
}

int main(void)
{
    rig_timer_init();
    /* Calibrate ticks -> instructions: the loop body is exactly 3 insns. */
    uint32_t cal_ticks = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
    printf("[cps1-qemu] integrated pipeline: 68000+bus, 3-layer BG+compositor, sound HLE "
           "(synthetic ROM/scene, no real CPS-1 ROM yet)\n");
    printf("[cps1-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal_ticks,
           (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

    uint64_t full = measure_path("full", cps1_core_run_frame, ipt_x1000);
    uint64_t device = measure_path("device-cost", cps1_core_run_frame_device_cost, ipt_x1000);
    uint64_t ltdc = measure_path("ltdc", run_frame_ltdc_total, ipt_x1000);
    /* Recompiler engine, same "ltdc" (real total) workload -- isolates
     * dispatch-mechanism cost alone, see measure_path_engine's comment. */
    uint64_t ltdc_rc = measure_path_engine("ltdc-rc", run_frame_ltdc_total, ipt_x1000,
                                            CPS1_ENGINE_RECOMPILER);

    printf("[cps1-qemu] summary: full(host-compositor)=%lu insn/frame, "
           "device-cost(SCROLL3+sprite only, historical)=%lu insn/frame, "
           "ltdc(REAL total: device-cost + SCROLL1+SCROLL2)=%lu insn/frame, saving vs full=%.1f%%\n",
           (unsigned long)full, (unsigned long)device, (unsigned long)ltdc,
           full ? 100.0 * (1.0 - (double)ltdc / (double)full) : 0.0);
    printf("[cps1-qemu] recompiler-vs-interpreter (same \"ltdc\" workload): "
           "interpreter=%lu insn/frame, recompiler=%lu insn/frame, dispatch saving=%.1f%%\n",
           (unsigned long)ltdc, (unsigned long)ltdc_rc,
           ltdc ? 100.0 * (1.0 - (double)ltdc_rc / (double)ltdc) : 0.0);
    printf("[cps1-qemu] NOTE: instruction count is a necessary, not sufficient, condition for "
           "60fps -- QEMU models neither cache misses nor flash wait states. The device's own "
           "DWT/frame ledger is the final judge.\n");
    return 0;
}
