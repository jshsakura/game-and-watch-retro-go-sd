#ifndef _RC_PROBE_H_
#define _RC_PROBE_H_

#include <stdint.h>

/* Compile gate. The Makefile passes -DRC_PROBE=1 when RC_PROBE=1; this default
 * keeps the include harmless in any translation unit (main.c, rg_main.c) when
 * the macro is unset, so the release build is byte-identical. */
#ifndef RC_PROBE
#define RC_PROBE 0
#endif

/* On-device feasibility probe for the 65816->C static recompiler's dispatch
 * path (map lookup + XIP site call) on the STM32H7B0.
 *
 * Runs the probe and HALTS only if the boot button combo is held at boot;
 * otherwise returns immediately so the normal boot path is unchanged. Mirrors
 * the gba_probe shape: always compiled, runtime-gated by a button combo,
 * measures with DWT cycle counters, reports on the LCD + printf log, then
 * loops forever refreshing the watchdog. */
void rc_probe_run_if_requested(uint32_t boot_buttons);

#endif /* _RC_PROBE_H_ */
