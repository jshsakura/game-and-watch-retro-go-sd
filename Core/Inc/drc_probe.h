#ifndef _DRC_PROBE_H_
#define _DRC_PROBE_H_

#include <stdint.h>

/* Compile gate. The Makefile passes -DDRC_PROBE=1 when DRC_PROBE=1; this
 * default keeps the include harmless in any translation unit (main.c,
 * rg_main.c) when the macro is unset, so the release build is byte-identical.
 * Mirrors rc_probe.h's shape exactly. */
#ifndef DRC_PROBE
#define DRC_PROBE 0
#endif

/* On-device instruction-FETCH feasibility gate for a PicoDrive SH-2 dynamic
 * recompiler port (explore/32x-drc-feasibility).
 *
 * The DRC's only viable code-cache placement is the ~73 KB free in the DTCM
 * stdlib heap (see drc_probe.c's top-of-file comment for the full budget
 * argument). Nobody should write 1500 lines of an ARM32->Thumb-2 backend
 * before knowing whether DTCM can even serve as a fast *instruction fetch*
 * region on this core -- ITCM is the zero-wait code path by design, DTCM is
 * wired to the CPU's D-Code bus (normally data-only), and RAM_EMU (AXI
 * SRAM) is I-cached. This probe copies byte-identical Thumb-2 machine code
 * into all three (plus, for free, measures the as-linked internal-flash
 * XIP baseline) and reports device DWT cycles/iteration for each.
 *
 * Runs and HALTS only if the boot button combo is held at boot; otherwise
 * returns immediately so the normal boot path is unchanged. Mirrors the
 * rc_probe.c shape: always compiled (when DRC_PROBE=1), runtime-gated by a
 * button combo, DWT cycle counters, reports on LCD + printf + one SD file,
 * then loops forever refreshing the watchdog. */
void drc_probe_run_if_requested(uint32_t boot_buttons);

#endif /* _DRC_PROBE_H_ */
