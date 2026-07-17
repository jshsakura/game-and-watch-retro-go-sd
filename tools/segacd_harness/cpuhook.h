/* cpuhook.h — enable gwenesis M68K HOOK_CPU path for the SegaCD histogram probe.
 *
 * The gwenesis Musashi (external/gwenesis/src/cpus/M68K/m68kcpu.c) already has
 * a fully-wired HOOK_CPU hook point at the top of the dispatch loop:
 *
 *     while (m68k.cycles < cycles) {
 *         ...
 *     #ifdef HOOK_CPU
 *         if (cpu_hook) cpu_hook(HOOK_M68K_E, 0, REG_PC, 0);
 *     #endif
 *         REG_IR = m68ki_read_imm_16();
 *         m68ki_instruction_jump_table[REG_IR]();
 *     }
 *
 * It also fires HOOK_M68K_R / HOOK_M68K_W from every memory read/write in
 * m68kcpu.h. None of those paths compile unless HOOK_CPU is defined AND a
 * cpuhook.h header is visible (m68k.h:46 #includes it). This file IS that
 * header — drop it on the -I path and add -DHOOK_CPU to the build.
 *
 * The hook is shared by the single global `m68k` (swapped between main/sub by
 * segacd_run_sub); scd_hist.c tells them apart via g_scd_hist_issub, which
 * segacd_engine.c sets around the sub-context swap.
 */
#ifndef CPUHOOK_H
#define CPUHOOK_H

#define HOOK_M68K_E 0   /* instruction execute: size=0, addr=PC, val=0       */
#define HOOK_M68K_R 1   /* memory read:      size=1/2/4, addr=off, val=data  */
#define HOOK_M68K_W 2   /* memory write:     size=1/2/4, addr=off, val=data  */

typedef void (*cpu_hook_fn)(int type, int size, unsigned int addr, unsigned int val);
extern cpu_hook_fn cpu_hook;

#endif /* CPUHOOK_H */
