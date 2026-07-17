/* ===========================================================================
 * rc_probe.c — on-device feasibility probe for the 65816->C static recompiler
 *              dispatch path on the STM32H7B0 (Game & Watch, SD-card variant).
 *
 * COMPILE GATE
 *   Guarded by RC_PROBE=1 (Makefile; default 0). The default release build
 *   does not compile this file — it is byte-identical to a tree without it.
 *   Enable: `make release DOCKER=1 RC_PROBE=1 <flags>`.
 *
 * WHY THIS EXISTS
 *   The recompiler translates SNES 65816 code to per-opcode C "site" functions.
 *   Dispatch is PC->site: each emulated opcode does (1) a map lookup keyed by
 *   the 24-bit PC, (2) an indirect call from RAM to a site function that must
 *   live in external flash (XIP) because the site code (1.1-1.5 MB) cannot fit
 *   RAM_EMU (724 KB), (3) the site body, (4) return. Dispatch runs ~1-2M/sec.
 *   The host PoC uses a FLAT 32 MB map read every opcode — impossible on device.
 *   This probe measures whether the on-device dispatch path meets the cycle
 *   budget on REAL hardware; the hazards (D-cache misses, flash wait states,
 *   the RAM<->XIP veneer at frequency) cannot be modeled by QEMU/a host build.
 *
 * STAGES
 *   Stage 1 (FULL — decisive for Constraint A): cycles/lookup for the dispatch
 *     map across placement x scheme x N x stream:
 *       placement : DTCM (0x20000000, 0-wait) vs AHB-SRAM (0x30000000, cached)
 *                   vs external flash (0x90000000, OSPI, cold D-cache)
 *       scheme    : open-addressing hash vs sorted-array binary search
 *       N         : 4355 (Zelda-scale) vs 8371 (large-ROM-scale)
 *       stream    : uniform-random (worst case) vs locality-realistic (~90%
 *                   intra-bank, mimics SNES tight loops + intra-bank calls)
 *     The PLACEMENT axis is the decisive one: it determines whether a D-cache
 *     miss per lookup blows the cycle budget.
 *
 *   Stage 2 (XIP veneer — stubbed on SD-card builds): measures the cost and
 *     correctness of calling K=64 site functions that execute from external
 *     flash (0x9001xxxx). The .xip_rcprobe linker section exists at a real
 *     EXTFLASH VMA, and the linker emits a long-branch veneer for direct BL
 *     from .text (confirmed via objdump). BUT on SD_CARD=1 builds extflash.bin
 *     is NOT flashed to the external flash chip (only intflash.bin is flashed;
 *     cores stream from SD at runtime). The bytes at 0x9001xxxx are therefore
 *     whatever the OFW/chainloader left there — NOT this section's code — and
 *     executing them would fault. Stage 2 stubs the runtime measurement with
 *     a clear reason. The veneer wiring is verified at link time (objdump).
 *     On non-SD builds (where extflash.bin IS flashed) the measurement could
 *     run; that is a TODO for a FLASH.ld build.
 *
 *   Stage 3 (combined per-opcode dispatch): hash-lookup (DTCM, the winning
 *     placement) + indirect call to a RAM-resident stub site + return. The
 *     call is RAM->RAM (NO flash veneer) because Stage 2 is stubbed, so this
 *     is a LOWER BOUND on dispatch cost. Compare against the SNES per-opcode
 *     budget (~240-480 cycles total emulation at 480 MHz / 1-2M dispatches-sec).
 *
 * WHAT EACH OUTCOME MEANS
 *   - map-in-DTCM cheap + (Stage2) veneer correct/fast => rc viable.
 *   - map-in-flash expensive => the map MUST live in DTCM (0x20000000); any
 *     cached/flash placement is a cache-miss-per-opcode catastrophe.
 *   - Stage2 runtime FAIL (if it could run) => the "DOOM XIP veneer" hazard
 *     is real at rc call frequency. On SD-card builds the hazard is that the
 *     code isn't in flash at all, which is a deployment constraint, not a
 *     hardware limit (SM/GBA prove flash exec works via the runtime cache).
 *
 * SHAPE (copied from gba_probe): always compiled (when RC_PROBE=1), RUNTIME-
 * gated by a boot button combo (returns immediately if not held), DWT cycle
 * counters, reports on LCD + printf, then halts in a watchdog-safe loop.
 * =========================================================================== */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "stm32h7xx_hal.h"
#include "gw_buttons.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "gittag.h"
#include "rc_probe.h"

#include "common.h"            /* common_emu_* DWT wrappers */
#include "odroid_overlay.h"
#include "odroid_display.h"
#include "odroid_colors.h"

#define RC_COMBO       (B_GAME | B_TIME)   /* no other boot hook uses this    */

/* ---- table sizes -------------------------------------------------------- */
#define RC_N_SMALL     4355U               /* Zelda-scale dispatch map        */
#define RC_N_LARGE     8371U               /* large-ROM-scale                 */
#define RC_SLOTS_SMALL 8192U               /* pow2, ~0.53 load factor (small) */
#define RC_SLOTS_LARGE 16384U              /* pow2, ~0.51 load factor (large) */

/* ---- measurement                                                        */
#define RC_LOOKUPS     100000U
#define RC_STREAM_LEN  8192U
#define RC_STREAM_MASK (RC_STREAM_LEN - 1U)
#define RC_READ_MASK   0x1FFFU            /* 8K-word flash read window        */
#define RC_BANK_SIZE   256U               /* locality-stream intra-bank span  */
#define RC_K_SITES     64U
#define RC_S3_CALLS    1000000U

/* ---- XIP site functions (Stage 2) --------------------------------------- *
 * Placed in .xip_rcprobe (EXTFLASH VMA 0x90010000). On SD-card builds these
 * bytes are NOT in external flash at boot (extflash.bin is not flashed), so
 * they cannot be executed — Stage 2 stubs the measurement. The functions
 * exist so the linker resolves .xip_rcprobe and emits a veneer for the
 * direct BL in rc_force_veneer(). */
#define RC_XIP_SITE(n) \
    void __attribute__((section(".xip_rcprobe"), noinline, used)) \
    rc_xip_site_##n(uint32_t sid, volatile uint32_t *ctrs, volatile uint32_t *snk) { \
        uint32_t a = sid * 2654435761u; \
        ctrs[sid]++; \
        *snk ^= a >> 16; \
    }

RC_XIP_SITE(0)  RC_XIP_SITE(1)  RC_XIP_SITE(2)  RC_XIP_SITE(3)

/* Function-pointer table — forces symbol retention and provides the
 * indirect-call path the real dispatch loop would use. */
typedef void (*rc_xip_fn)(uint32_t, volatile uint32_t *, volatile uint32_t *);
static rc_xip_fn const rc_xip_table[] = {
    rc_xip_site_0, rc_xip_site_1, rc_xip_site_2, rc_xip_site_3,
};

static volatile uint32_t g_sink;

/* ---- helpers ------------------------------------------------------------ */

static void cache_drop(void)
{
    SCB_InvalidateDCache();   /* discard: probe buffers are read-only during measure */
    __DSB();
    __ISB();
}

static uint16_t rc_y;
static void rc_line(uint16_t color, const char *fmt, ...)
{
    char buf[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("rc: %s\n", buf);
    if (rc_y < GW_LCD_HEIGHT)
        rc_y = (uint16_t)odroid_overlay_draw_text(0, rc_y, GW_LCD_WIDTH, buf, color, C_BLACK);
}

/* ---- open-addressing hash (shared across placements) -------------------- *
 * Knuth multiplicative hash (single multiply). The per-placement COST does
 * not depend on the hash function, only on where keys/ids live in memory. */
static uint16_t __attribute__((noinline)) rc_hash_lookup(
    const uint32_t *keys, const uint16_t *ids, uint32_t mask, uint32_t kpc)
{
    uint32_t h = (kpc * 2654435761u) >> 13;
    for (;;) {
        h &= mask;
        uint32_t k = keys[h];
        if (k == kpc)      return ids[h];
        if (k == 0xFFFFFFFFu) return 0;
        h++;
    }
}

static void rc_build_hash(
    uint32_t *keys, uint16_t *ids, uint32_t slots, uint32_t mask, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < slots; i++) { keys[i] = 0xFFFFFFFFu; ids[i] = 0; }
    for (i = 0; i < n; i++) {
        uint32_t k = i;                 /* synthetic distinct PCs */
        uint32_t h = ((k * 2654435761u) >> 13) & mask;
        while (keys[h] != 0xFFFFFFFFu) h = (h + 1u) & mask;
        keys[h] = k;
        ids[h]  = (uint16_t)(i + 1u);
    }
}

/* ---- sorted-array binary search ----------------------------------------- */
static uint16_t __attribute__((noinline)) rc_bsearch(
    const uint32_t *keys, const uint16_t *ids, uint32_t n, uint32_t kpc)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (keys[mid] < kpc) lo = mid + 1u;
        else                 hi = mid;
    }
    if (lo < n && keys[lo] == kpc) return ids[lo];
    return 0;
}

/* sorted: keys[i] = i, ids[i] = i+1 (already sorted for synthetic PCs) */
static void rc_build_sorted(uint32_t *keys, uint16_t *ids, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) { keys[i] = i; ids[i] = (uint16_t)(i + 1u); }
}

/* ---- measurement wrappers ----------------------------------------------- */

static uint32_t meas_hash(
    const uint32_t *keys, const uint16_t *ids, uint32_t mask,
    const uint32_t *stream, int cold)
{
    uint32_t acc = 0, cyc, i;
    if (cold) cache_drop();
    common_emu_clear_dwt_cycles();
    for (i = 0; i < RC_LOOKUPS; i++)
        acc ^= rc_hash_lookup(keys, ids, mask, stream[i & RC_STREAM_MASK]);
    cyc = common_emu_get_dwt_cycles();
    g_sink ^= acc;
    return cyc / RC_LOOKUPS;
}

static uint32_t meas_bsearch(
    const uint32_t *keys, const uint16_t *ids, uint32_t n,
    const uint32_t *stream, int cold)
{
    uint32_t acc = 0, cyc, i;
    if (cold) cache_drop();
    common_emu_clear_dwt_cycles();
    for (i = 0; i < RC_LOOKUPS; i++)
        acc ^= rc_bsearch(keys, ids, n, stream[i & RC_STREAM_MASK]);
    cyc = common_emu_get_dwt_cycles();
    g_sink ^= acc;
    return cyc / RC_LOOKUPS;
}

/* raw read latency over a placement (volatile base forces real loads) */
static uint32_t meas_read(const volatile uint32_t *base, uint32_t mask,
                          const uint32_t *stream)
{
    uint32_t acc = 0, cyc, i;
    cache_drop();
    common_emu_clear_dwt_cycles();
    for (i = 0; i < RC_LOOKUPS; i++)
        acc ^= base[stream[i & RC_STREAM_MASK] & mask];
    cyc = common_emu_get_dwt_cycles();
    g_sink ^= acc;
    return cyc / RC_LOOKUPS;
}

/* ---- stream generators -------------------------------------------------- */

static void rc_gen_uniform(uint32_t *stream, uint32_t max_key)
{
    uint32_t s = 0x12345678u, i;
    for (i = 0; i < RC_STREAM_LEN; i++) {
        s = s * 1664525u + 1013904223u;
        stream[i] = s % max_key;
    }
}

/* locality-realistic: ~87% of lookups stay within a 256-entry "bank"
 * (mimics SNES tight loops + intra-bank calls); ~13% jump to a new bank. */
static void rc_gen_locality(uint32_t *stream, uint32_t max_key)
{
    uint32_t s = 0xABCDEF01u, base = 0, i;
    for (i = 0; i < RC_STREAM_LEN; i++) {
        s = s * 1664525u + 1013904223u;
        if ((s & 0xF) < 14) {            /* ~87% within bank */
            stream[i] = (base + (s >> 4) % RC_BANK_SIZE) % max_key;
        } else {                          /* ~13% bank switch */
            s = s * 1664525u + 1013904223u;
            base = s % max_key;
            stream[i] = base;
        }
    }
}

/* ---- Stage 3 RAM-resident stub site (NO flash veneer) ------------------- */
static uint32_t *g_site_ctrs;               /* arena */
static rc_xip_fn volatile *g_site_tbl;      /* arena */
static void __attribute__((noinline)) rc_stub_site(
    uint32_t sid, volatile uint32_t *ctrs, volatile uint32_t *snk)
{
    uint32_t a = sid ^ 0x9E3779B9u;
    ctrs[sid]++;
    *snk ^= (a << 5) | (a >> 27);
}

/* ---- Force linker veneer for objdump verification ----------------------- *
 * Never executed (trigger is always 0); exists solely so .text contains a
 * direct BL to 0x9001xxxx, which the linker MUST bridge with a long-branch
 * veneer (BL range is +/-16 MB; 0x90010000 is ~2 GB from .text at 0x081xxxxx).
 * The veneer is the artifact Stage 2 would measure if the code were in flash. */
static volatile uint32_t rc_veneer_trigger;
static void __attribute__((noinline)) rc_force_veneer(void)
{
    if (rc_veneer_trigger == 0xDEAD1234u)
        rc_xip_site_0(0, g_site_ctrs, &g_sink);
}

/* ===========================================================================
 *  Main probe entry
 * =========================================================================== */
void rc_probe_run_if_requested(uint32_t boot_buttons)
{
    extern void * __RAM_EMU_START__[];
    uint8_t *arena = (uint8_t *)*__RAM_EMU_START__;
    uint32_t *stream_u, *stream_l;     /* uniform + locality streams (arena)  */
    uint32_t *dtcm_k;                 /* DTCM keys (reused: hash + bsearch)  */
    uint16_t *dtcm_v;                 /* DTCM ids  (reused: hash + bsearch)  */
    uint32_t *ahb_k;                  /* AHB  keys (reused: hash + bsearch)  */
    uint16_t *ahb_v;                  /* AHB  ids  (reused: hash + bsearch)  */
    uint32_t ok, sum, i;
    /* Route the linker marker through a volatile uintptr_t so the compiler
     * cannot track array bounds from the 1-byte __EXTFLASH_START__ object. */
    static volatile uintptr_t extaddr;
    const volatile uint32_t *ext;
    uint32_t c_dtcm_h, c_ahb_h, c_flash;
    uint32_t c_dtcm_b, c_ahb_b;

    if ((boot_buttons & RC_COMBO) != RC_COMBO) return;  /* inert otherwise */

    extaddr = (uintptr_t)&__EXTFLASH_START__;
    ext = (const volatile uint32_t *)extaddr;

    lcd_backlight_off();
    odroid_display_set_backlight(ODROID_BACKLIGHT_LEVEL6);
    common_emu_enable_dwt_cycles();

    /* ---- allocate streams in arena ---------------------------------------- */
    stream_u = (uint32_t *)arena;
    stream_l = stream_u + RC_STREAM_LEN;
    rc_gen_uniform(stream_u, RC_N_SMALL);
    rc_gen_locality(stream_l, RC_N_SMALL);

    /* ---- allocate DTCM measurement buffers (heap is in DTCMRAM) ----------- *
     * One reusable set: max(RC_SLOTS_SMALL, RC_N_LARGE) = max(8192, 8371) = 8371.
     * keys: 8371*4 = 33 KB. ids: 8371*2 = 16 KB. Total ~49 KB (fits 81 KB). */
    dtcm_k = (uint32_t *)malloc(sizeof(uint32_t) * RC_N_LARGE);
    dtcm_v = (uint16_t *)malloc(sizeof(uint16_t) * RC_N_LARGE);

    /* ---- allocate AHB measurement buffers -------------------------------- *
     * Same sizes. ahb_init() resets the bump allocator to __ahbram_heap_start__. */
    ahb_init();
    ahb_k = (uint32_t *)ahb_only_malloc(sizeof(uint32_t) * RC_N_LARGE);
    ahb_v = (uint16_t *)ahb_only_malloc(sizeof(uint16_t) * RC_N_LARGE);

    /* ---- site tables for Stage 3 (arena, after streams) ------------------- */
    g_site_ctrs = (uint32_t *)(stream_l + RC_STREAM_LEN);
    g_site_tbl  = (rc_xip_fn volatile *)(g_site_ctrs + RC_K_SITES);

    lcd_sync(); lcd_reset_active_buffer(); rc_y = 0;
    rc_line(C_GREEN, "RC PROBE %s", GIT_TAG);
    rc_line(C_WHITE, "DTCM@%p AHB@%p", (void*)dtcm_k, (void*)ahb_k);

    /* ---- correctness self-check: hash + bsearch resolve every key --------- */
    {
        uint32_t mask_s = RC_SLOTS_SMALL - 1u;
        rc_build_hash(dtcm_k, dtcm_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        ok = 0;
        for (i = 0; i < RC_N_SMALL; i++)
            if (rc_hash_lookup(dtcm_k, dtcm_v, mask_s, i) == (uint16_t)(i + 1u)) ok++;
        rc_line(ok == RC_N_SMALL ? C_GREEN : C_RED, "hash %lu/%lu %s",
                (unsigned long)ok, (unsigned long)RC_N_SMALL,
                ok == RC_N_SMALL ? "PASS" : "FAIL");
    }
    {
        rc_build_sorted(dtcm_k, dtcm_v, RC_N_SMALL);
        ok = 0;
        for (i = 0; i < RC_N_SMALL; i++)
            if (rc_bsearch(dtcm_k, dtcm_v, RC_N_SMALL, i) == (uint16_t)(i + 1u)) ok++;
        rc_line(ok == RC_N_SMALL ? C_GREEN : C_RED, "bsearch %lu/%lu %s",
                (unsigned long)ok, (unsigned long)RC_N_SMALL,
                ok == RC_N_SMALL ? "PASS" : "FAIL");
    }
    lcd_sync();

    /* =======================================================================
     *  Stage 1: map lookup cost — placement x scheme x N x stream
     * ======================================================================= */
    printf("rc: === Stage 1: map lookup cost (cyc/lookup, cold) ===\n");

    /* --- hash, N=4355, DTCM --- */
    {
        uint32_t mask_s = RC_SLOTS_SMALL - 1u;
        rc_build_hash(dtcm_k, dtcm_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        c_dtcm_h = meas_hash(dtcm_k, dtcm_v, mask_s, stream_u, 1);
        printf("rc: S1 hash N=%u DTCM uniform=%lu\n", RC_N_SMALL, (unsigned long)c_dtcm_h);
        c_dtcm_h = meas_hash(dtcm_k, dtcm_v, mask_s, stream_l, 1);
        printf("rc: S1 hash N=%u DTCM locality=%lu\n", RC_N_SMALL, (unsigned long)c_dtcm_h);
    }
    /* --- hash, N=4355, AHB --- */
    {
        uint32_t mask_s = RC_SLOTS_SMALL - 1u;
        rc_build_hash(ahb_k, ahb_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        c_ahb_h = meas_hash(ahb_k, ahb_v, mask_s, stream_u, 1);
        printf("rc: S1 hash N=%u AHB uniform=%lu\n", RC_N_SMALL, (unsigned long)c_ahb_h);
        c_ahb_h = meas_hash(ahb_k, ahb_v, mask_s, stream_l, 1);
        printf("rc: S1 hash N=%u AHB locality=%lu\n", RC_N_SMALL, (unsigned long)c_ahb_h);
    }
    /* --- hash, N=8371: SKIPPED ----------------------------------------------- *
     * A 16384-slot open-addressing table needs 96 KB — exceeds both the 81 KB
     * DTCM heap and the ~86 KB AHB heap. The N axis for hash is covered by
     * N=4355 (8192 slots, 48 KB) in both placements. N=8371 is tested with
     * binary search below (50 KB, fits both). */
    printf("rc: S1 hash N=%u: SKIPPED (16384-slot table exceeds DTCM+AHB heaps)\n",
           (unsigned)RC_N_LARGE);

    /* --- binary search, N=4355, DTCM --- */
    {
        rc_build_sorted(dtcm_k, dtcm_v, RC_N_SMALL);
        c_dtcm_b = meas_bsearch(dtcm_k, dtcm_v, RC_N_SMALL, stream_u, 1);
        printf("rc: S1 bsearch N=%u DTCM uniform=%lu\n", RC_N_SMALL, (unsigned long)c_dtcm_b);
        c_dtcm_b = meas_bsearch(dtcm_k, dtcm_v, RC_N_SMALL, stream_l, 1);
        printf("rc: S1 bsearch N=%u DTCM locality=%lu\n", RC_N_SMALL, (unsigned long)c_dtcm_b);
    }
    /* --- binary search, N=4355, AHB --- */
    {
        rc_build_sorted(ahb_k, ahb_v, RC_N_SMALL);
        c_ahb_b = meas_bsearch(ahb_k, ahb_v, RC_N_SMALL, stream_u, 1);
        printf("rc: S1 bsearch N=%u AHB uniform=%lu\n", RC_N_SMALL, (unsigned long)c_ahb_b);
        c_ahb_b = meas_bsearch(ahb_k, ahb_v, RC_N_SMALL, stream_l, 1);
        printf("rc: S1 bsearch N=%u AHB locality=%lu\n", RC_N_SMALL, (unsigned long)c_ahb_b);
    }
    /* --- binary search, N=8371, DTCM --- */
    {
        rc_build_sorted(dtcm_k, dtcm_v, RC_N_LARGE);
        c_dtcm_b = meas_bsearch(dtcm_k, dtcm_v, RC_N_LARGE, stream_u, 1);
        printf("rc: S1 bsearch N=%u DTCM uniform=%lu\n", RC_N_LARGE, (unsigned long)c_dtcm_b);
        c_dtcm_b = meas_bsearch(dtcm_k, dtcm_v, RC_N_LARGE, stream_l, 1);
        printf("rc: S1 bsearch N=%u DTCM locality=%lu\n", RC_N_LARGE, (unsigned long)c_dtcm_b);
    }
    /* --- binary search, N=8371, AHB --- */
    {
        rc_build_sorted(ahb_k, ahb_v, RC_N_LARGE);
        c_ahb_b = meas_bsearch(ahb_k, ahb_v, RC_N_LARGE, stream_u, 1);
        printf("rc: S1 bsearch N=%u AHB uniform=%lu\n", RC_N_LARGE, (unsigned long)c_ahb_b);
        c_ahb_b = meas_bsearch(ahb_k, ahb_v, RC_N_LARGE, stream_l, 1);
        printf("rc: S1 bsearch N=%u AHB locality=%lu\n", RC_N_LARGE, (unsigned long)c_ahb_b);
    }
    /* --- external flash: raw read latency (proxy for any flash-resident map) --- */
    {
        c_flash = meas_read(ext, RC_READ_MASK, stream_u);
        printf("rc: S1 flash-read uniform=%lu\n", (unsigned long)c_flash);
        c_flash = meas_read(ext, RC_READ_MASK, stream_l);
        printf("rc: S1 flash-read locality=%lu\n", (unsigned long)c_flash);
    }

    /* LCD summary (details went to printf) */
    {
        uint32_t mask_s = RC_SLOTS_SMALL - 1u;
        rc_build_hash(dtcm_k, dtcm_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        c_dtcm_h = meas_hash(dtcm_k, dtcm_v, mask_s, stream_u, 1);
        rc_build_hash(ahb_k, ahb_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        c_ahb_h  = meas_hash(ahb_k, ahb_v, mask_s, stream_u, 1);
        c_flash  = meas_read(ext, RC_READ_MASK, stream_u);
        rc_line(C_WHITE, "S1 h4355 DTCM:%lu AHB:%lu", (unsigned long)c_dtcm_h, (unsigned long)c_ahb_h);
        rc_line(C_WHITE, "S1 flash-read:%lu c/lu", (unsigned long)c_flash);
    }
    lcd_sync();

    /* =======================================================================
     *  Stage 2: XIP veneer call cost + correctness
     * ======================================================================= *
     * The .xip_rcprobe section is linked at 0x90010000 (EXTFLASH VMA). On
     * SD_CARD=1 builds extflash.bin is NOT flashed to the external flash chip,
     * so the bytes at 0x90010000 are not this section's code. Executing them
     * would fault or produce garbage. We therefore stub the measurement.
     *
     * The linker veneer IS emitted (verified via objdump of the ELF) — a
     * direct BL from rc_force_veneer (.text) to rc_xip_site_0 (.xip_rcprobe)
     * crosses the +/-16 MB BL range, forcing a long-branch stub. That stub is
     * the artifact whose cost Stage 2 would measure if the code were in flash.
     *
     * TODO: run this measurement on a non-SD build (SD_CARD=0, FLASH.ld) where
     * extflash.bin IS flashed, or after manually flashing extflash.bin.
     */
    rc_force_veneer();   /* never calls through (trigger==0); keeps veneer live */
    rc_line(C_YELLOW, "S2 SKIP extflash-not-flashed");
    printf("rc: S2 SKIP: SD-card builds do not flash extflash.bin to the flash chip.\n");
    printf("rc:   .xip_rcprobe at %p (VMA 0x9001xxxx) — verify veneer via objdump.\n",
           (void*)rc_xip_table[0]);
    lcd_sync();

    /* =======================================================================
     *  Stage 3: combined per-opcode dispatch (LOWER BOUND — no flash veneer)
     * ======================================================================= */
    {
        uint32_t mask_s = RC_SLOTS_SMALL - 1u;
        uint32_t per_op, m = 0, j;
        rc_build_hash(dtcm_k, dtcm_v, RC_SLOTS_SMALL, mask_s, RC_N_SMALL);
        for (i = 0; i < RC_K_SITES; i++) { g_site_ctrs[i] = 0; g_site_tbl[i] = rc_stub_site; }
        cache_drop();
        common_emu_clear_dwt_cycles();
        for (j = 0; j < RC_S3_CALLS; j++) {
            uint16_t id = rc_hash_lookup(dtcm_k, dtcm_v, mask_s, stream_u[j & RC_STREAM_MASK]);
            uint32_t sid = id % RC_K_SITES;
            g_site_tbl[sid](sid, g_site_ctrs, &g_sink);   /* forced indirect call */
            m++;
        }
        per_op = common_emu_get_dwt_cycles() / RC_S3_CALLS;
        for (i = 0, sum = 0; i < RC_K_SITES; i++) sum += g_site_ctrs[i];
        rc_line(sum == m ? C_GREEN : C_RED,
                "S3 %lu/%lu %s %lu c/op (LB)",
                (unsigned long)sum, (unsigned long)m,
                sum == m ? "ok" : "FAIL", (unsigned long)per_op);
        printf("rc: S3 combined dispatch: %lu cyc/opcode (lower bound, no flash veneer)\n",
               (unsigned long)per_op);
        printf("rc:   SNES budget ~240-480 cyc/op total; dispatch fraction = %.1f%%\n",
               per_op * 100.0f / 360.0f);
        lcd_sync();
    }

    printf("rc: halt (probe done)\n");
    for (;;) { wdog_refresh(); HAL_Delay(50); }
}
