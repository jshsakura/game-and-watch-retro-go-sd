#ifndef _MD32X_PROFILE_H_
#define _MD32X_PROFILE_H_

/* Device-side DWT phase profiler for 32X (picodrive). Enabled only when the
 * build is invoked with MD32X_DEVICE_PROFILE=1 — see Makefile. The
 * implementation lives in md32x_profile.c (NOT main_md32x.c) so the linker
 * sends its .text/.rodata to .xip_md32x / .rodata_md32x (QSPI flash) instead
 * of the RAM_EMU overlay. Only its small .bss (pp_counters + refcounts, ~156 B)
 * stays in overlay — that fits the baseline headroom (188 B). Keeping the
 * heavy snprintf/fopen code in main_md32x.o overflows the overlay by ~1.1 KB
 * because main_md32x.o's .text is explicitly forced into overlay RAM by the
 * linker script. */
#ifdef MD32X_DEVICE_PROFILE

/* Run 120 headless PicoFrame() iterations with all pprof probes accumulating
 * into pp_counters, then write per-bucket averages + % of pp_frame to
 * /32x_dwt.txt. Boot-time SD write only (HARD RULE: never in the main loop).
 * Caller MUST have completed PicoInit + warm-up frame + set_out_buffer first.
 * wdog_refresh() is called every iteration (~2s of emulation at 60 fps — the
 * watchdog window is ~472 ms). */
void md32x_run_profile(void);

#endif /* MD32X_DEVICE_PROFILE */

#endif /* _MD32X_PROFILE_H_ */
