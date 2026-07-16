/* Sega 32X (picodrive) on QEMU's Cortex-M7 (mps2-an500): executed-instruction
 * count per frame on a real ARMv7-M stream. Phase-2 feasibility proof for the
 * dual-SH-2 32X: is the interpreter's HOST cost within the G&W budget?
 *
 * picodrive core (interpreter, use_sh2drc=0) driven bare-metal: PicoInit ->
 * PicoLoadMedia(32X) -> PicoFrame() per frame, wrapped in CMSDK-timer reads.
 * ROM linked in (_binary_rom_32x_start) and passed straight to the loader.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pico/pico_types.h"
#include "pico/pico.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 600
#endif
#define RIG_WINDOW 20

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* picodrive SH-2 instruction counter (cpu/sh2/mame/sh2pico.c) */
extern unsigned long long g_sh2_insns;

/* ROM blob (objcopy .rom section) */
extern const unsigned char _binary_rom_32x_start[];
extern const unsigned char _binary_rom_32x_end[];

/* ---- platform stubs the core references (DRC-side, harmless here) ---- */
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) {
    (void)addr; (void)need_exec; (void)is_fixed;
    return malloc(size);
}
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) {
    (void)oldsize; return realloc(ptr, newsize);
}
void plat_munmap(void *ptr, size_t size) { (void)size; free(ptr); }
int plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }

/* output buffer for the MD/32X renderer */
static uint16_t s_fb[320 * 240];
/* sound output buffer — REQUIRED: with sndRate=0 the PWM scheduler leaves
 * pwm.cycles=0 and pwm.c's `while (sh2_cycles_diff >= pwm.cycles)` spins the
 * frame forever. Enabling sound sets pwm.cycles; output is measured, not played. */
static short s_snd[2 * 54000 / 50 + 8];

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);   /* bare-metal: flush every printf */
    uint32_t rom_len = (uint32_t)(_binary_rom_32x_end - _binary_rom_32x_start);

    rig_timer_init();
    uint32_t cal = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal ? cal : 1));
    printf("[32x-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal, (unsigned long)(ipt_x1000/1000), (unsigned long)(ipt_x1000%1000));
    printf("[32x-qemu] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

    PicoInit();
    PicoIn.opt |= POPT_EN_32X | POPT_ACC_SPRITES | POPT_EN_STEREO;
    PicoIn.sndRate = 44100;          /* MUST be nonzero: sets pwm.cycles (else PWM spins) */
    PicoIn.sndOut  = s_snd;

    enum media_type_e mt = PicoLoadMedia("game.32x", _binary_rom_32x_start, rom_len,
                                         "carthw.cfg", NULL, NULL, NULL);
    printf("[32x-qemu] PicoLoadMedia -> media_type=%d AHW=%x\n", (int)mt, (unsigned)PicoIn.AHW);
    if (mt != PM_ERROR) {
        /* ok */
    } else {
        printf("[32x-qemu] LOAD FAILED\n");
        return 3;
    }

    /* The frontend (not the core) enables 32X from the ROM header; do it here. */
    { extern void Pico32xStartup(void); Pico32xStartup(); }  /* known-32X ROM */
    /* CRITICAL: sets msh2/ssh2.mult_m68k_to_sh2. If 0 (uninitialized) the frame
     * scheduler tells the SH-2s to run 0 cycles -> they never execute and the
     * 68000 spins forever waiting for them = the real hang. */
    { extern void Pico32xPrepare(void); Pico32xPrepare(); }
    printf("[32x-qemu] after startup+prepare AHW=%x\n", (unsigned)PicoIn.AHW);
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(s_fb, 320 * 2);
    PicoReset();

    /* CRITICAL: the frontend (libretro.c) calls this after load and it is what
     * initializes the frame-loop timing (scanline counters, PsndLen). Without
     * it PicoFrame()'s scheduler runs on uninitialized timing and the first
     * frame never returns — the hang the previous pass hit. */
    PicoLoopPrepare();
    PsndRerate(0);                   /* sets PsndLen + pwm.cycles for the frame scheduler */
    /* AFTER reset: sets msh2/ssh2.mult_m68k_to_sh2. If 0, run_sh2 executes 0
     * SH-2 cycles, m68krcycles_done never advances, and the frame scheduler's
     * `while (CYCLES_GT(m68k_target, now))` spins forever. THE hang. Called
     * here (post-reset) because PicoReset re-zeros the SH-2 clock state. */
    { extern void Pico32xPrepare(void); Pico32xPrepare(); }

    uint64_t tot = 0, mn = ~0ull, mx = 0, sh2_tot = 0;
    unsigned long long sh2_prev = g_sh2_insns;

    for (int f = 0; f < RIG_FRAMES; f++) {
        uint32_t t0 = rig_timer_now();
        PicoFrame();
        uint32_t t1 = rig_timer_now();
        uint32_t ticks = (uint32_t)(t1 - t0);
        uint64_t insn = (uint64_t)ticks * ipt_x1000 / 1000;

        unsigned long long sh2_now = g_sh2_insns, sh2_d = sh2_now - sh2_prev;
        sh2_prev = sh2_now;

        if ((f % 5) == 0) printf("  f%04d host=%lu sh2=%llu\n", f, (unsigned long)insn, sh2_d);
        if (f >= 20) {                 /* skip boot */
            tot += insn; sh2_tot += sh2_d;
            if (insn < mn) mn = insn;
            if (insn > mx) mx = insn;
            if ((f + 1) % RIG_WINDOW == 0)
                printf("  f%04d host=%lu insn/frame  sh2=%llu\n",
                       f + 1, (unsigned long)insn, sh2_d);
        }
    }
    int n = RIG_FRAMES - 60;
    printf("[32x-qemu] done %d frames  avg host=%lu  min=%lu  max=%lu insn/frame  (avg sh2=%llu)\n",
           n, (unsigned long)(n>0?tot/n:0), (unsigned long)mn, (unsigned long)mx,
           (unsigned long long)(n>0?sh2_tot/n:0));
    return 0;
}
