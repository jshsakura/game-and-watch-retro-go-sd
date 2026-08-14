/* Device-side DWT phase profiler for 32X (picodrive) — public interface.
 * See md32x_profile.c for the full design writeup (why a separate TU, the
 * disjoint bucket scheme, AHB pool placement, byte-identical guarantee). */
#pragma once

#ifdef MD32X_DEVICE_PROFILE

#include <stdint.h>
#include <stdbool.h>

/* Arms the DWT cycle counter's delta pools (AHB-allocated). Call once before
 * the main loop starts, after any boot-time SD writes are sealed. */
void md32x_profile_init(void);

/* Records one main-loop iteration's disjoint phase deltas and, once
 * MD32X_PROFILE_FRAMES frames have been recorded, performs the one-shot
 * dump to /32x_dwt.txt. Call once per iteration, after the final (audio)
 * DWT read. t_pace..t_audio are CUMULATIVE reads taken from a single
 * common_emu_clear_dwt_cycles() at the top of that same iteration, in this
 * order: pace (after common_emu_frame_loop), proc (after input/pad/
 * out-buffer setup), pico (after PicoFrame), blit (after overlay+lcd_swap),
 * audio (after common_emu_sound_sync) — the caller does not compute deltas
 * itself, just passes the raw cumulative reads in order. */
void md32x_profile_record(bool drawFrame, uint32_t t_pace, uint32_t t_proc,
                           uint32_t t_pico, uint32_t t_blit, uint32_t t_audio);

#endif /* MD32X_DEVICE_PROFILE */
