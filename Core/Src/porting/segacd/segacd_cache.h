/* segacd_cache.h — $7c80 rotation-coefficient delta cache for SegaCD firmware.
 *
 * Compiled when SCD_CACHE and HOOK_CPU are both defined.  Provides a
 * cpu_hook implementation that intercepts the sub-68K rotation-calc routine
 * at $7c80 and applies cached deltas instead of recomputing.
 *
 * The cache is DELTA-BASED: $7c80 has read-modify-write accumulators at
 * A5+$40 and A5+$64 that change every call, but the deltas are a pure
 * function of the input parameters (A5+$00-$3F) which are identical for
 * all stamp calls within a frame and across 2/3 of frames.  Verified with
 * 0 mismatches over 1286 checks in the host harness.
 *
 * Hook toggle: call scd_cache_hook_enable(1) after loading the sub-68K
 * context, scd_cache_hook_enable(0) after swapping back to main.  This
 * eliminates per-instruction overhead for the ~10x more main-68K insns.
 */
#ifndef SEGACD_CACHE_H
#define SEGACD_CACHE_H

#include <stdint.h>

/* Enable/disable the per-instruction hook.  When disabled (0), the global
 * cpu_hook pointer is set to NULL and the dispatch loop skips the call
 * entirely.  Call with 1 after sub-68K context swap-in, 0 after swap-out. */
void scd_cache_hook_enable(int enable);

/* Read cumulative hit/miss counters (for diagnostics). */
void scd_cache_stats(uint32_t *hits, uint32_t *misses);

#endif /* SEGACD_CACHE_H */
