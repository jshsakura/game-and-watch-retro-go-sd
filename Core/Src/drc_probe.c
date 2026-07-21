/* ===========================================================================
 * drc_probe.c — on-device INSTRUCTION-FETCH feasibility gate for a PicoDrive
 *               SH-2 dynamic recompiler port (explore/32x-drc-feasibility).
 *
 * COMPILE GATE
 *   Guarded by DRC_PROBE=1 (Makefile; default 0). The default release build
 *   does not compile this file -- it is byte-identical to a tree without it.
 *   Enable: `make release DOCKER=1 DRC_PROBE=1 <flags>`.
 *
 * WHY THIS EXISTS (the cheap gate before the expensive port)
 *   The 32X SH-2 interpreter costs ~100.8 device cycles per guest
 *   instruction (msh2 at 59.5% of the frame, device-measured). Inlining the
 *   opcode fetch bought nothing, ruling out call/dispatch overhead and
 *   pointing at memory stalls: 256 KB guest SDRAM working set against a
 *   16 KB D-cache. The one remaining real lever is PicoDrive's SH-2 dynamic
 *   recompiler (external/picodrive/cpu/drc) -- but it emits ARM32 and this
 *   MCU executes Thumb-2 ONLY (a new backend), and it wants a multi-MB code
 *   cache when the only free RAM here is ~73 KB in the DTCM stdlib heap
 *   (device log: "heap used=17616/91024").
 *
 *   Before anyone writes ~1500 lines of an ARM32->Thumb-2 DRC backend, this
 *   probe answers ONE question with a device measurement: how fast is
 *   INSTRUCTION FETCH from DTCM (0x20000000) compared to ITCM (0x00000000,
 *   the zero-wait code path every other hot loop in this firmware already
 *   uses) and RAM_EMU (0x24000000+, AXI SRAM, I-cached)? DTCM is wired to
 *   the Cortex-M7's D-Code bus, which is architecturally a DATA path, not
 *   the I-Code bus ITCM uses -- whether the core can even fetch code from
 *   there at reasonable speed (or fetch it AT ALL without faulting) is an
 *   open question that no amount of reading the TRM substitutes for a real
 *   measurement on real silicon. If DTCM instruction fetch is much slower
 *   than ITCM (or faults outright), a 73 KB DTCM code cache does not pay
 *   and the whole design has to change before the expensive port starts.
 *
 * METHOD
 *   drc_probe_body (below) is ONE hand-written Thumb-2 routine: a tight
 *   15-instruction dependent ALU chain closed by a backward conditional
 *   branch, i.e. deliberately shaped like a DRC-emitted basic block (dense
 *   ALU ops, one loop-closing branch) rather than a trivial NOP loop that a
 *   tiny loop buffer could hide region latency behind. It is assembled ONCE
 *   with a plain `.thumb` (NOT `.thumb_func`) directive so the linker does
 *   not tag its start/end symbols with the Thumb bit -- that keeps
 *   `end - start` an exact byte count. The SAME bytes are then memcpy'd
 *   unmodified into an ITCM buffer, a DTCM buffer (plain malloc -- this
 *   codebase's stdlib heap IS the DTCM heap, see gw_malloc.c's dtcm_malloc)
 *   and a RAM_EMU buffer (nothing is loaded into RAM_EMU yet at this boot
 *   point). Each destination is called through a function pointer with the
 *   Thumb bit manually OR'd in (mandatory on ARMv7-M: BX/BLX to an
 *   even address is a UsageFault, there is no ARM state on Cortex-M to fall
 *   back to). The as-linked copy in this TU's own .text (-> internal flash,
 *   XIP) is measured too, for free, as a fourth reference point -- no copy
 *   needed, just call it directly.
 *
 *   Each placement is measured twice: COLD (I-cache + D-cache invalidated
 *   immediately before the call -- first-touch cost) and WARM (called again
 *   right after, region now cache-hot for the cacheable placements). The
 *   delta between them is the cache-miss penalty for that placement; ITCM
 *   and DTCM are not cacheable (direct-connected TCMs) so cold/warm should
 *   read the same for them -- if they do not, that is itself informative.
 *
 *   A correctness pass (fixed iters/seed, same deterministic arithmetic
 *   everywhere) cross-checks all four placements return the IDENTICAL
 *   accumulator value before any cycle number is trusted -- a wrong Thumb
 *   bit, a bad copy, or a cache-coherency bug would show up as a mismatch,
 *   not as a plausible-looking wrong number.
 *
 * CRASH IS DATA, NOT FAILURE
 *   If DTCM instruction fetch is not just slow but architecturally
 *   unsupported on this core, the DTCM call can Hardfault the device
 *   outright (SCB->SHCSR is never enabled in this firmware -- see
 *   CLAUDE.md's "Debugging crashes on hardware" section -- so it will show
 *   as a bare "Hardfault" BSOD, no further detail). That crash IS the
 *   answer: it rules out the DTCM-cache design more decisively than any
 *   slow-but-working cycle count could. Because of this, the SD report is
 *   NOT a single one-shot write at the end (unlike md32x_profile.c, which
 *   deliberately writes once to avoid disturbing frame pacing -- not a
 *   concern here, there is no pacing). Instead each region's result is
 *   appended to /32x_drc_probe.txt the moment it is known (open "ab",
 *   write, fclose -- the exact pattern md32x_profile.c's own boot-time diag
 *   writes already use), so a mid-run crash still leaves every
 *   already-measured region on the SD card. Regions run in the order
 *   ITCM, DTCM, RAM_EMU, INTFLASH -- DTCM (the one actually in question)
 *   second, right after the known-safe ITCM baseline lands.
 *
 * SHAPE: copied from rc_probe.c (this repo's other on-device feasibility
 *   gate) -- always compiled (when DRC_PROBE=1), runtime-gated by a boot
 *   button combo, DWT cycle counters, reports on LCD + printf + SD file,
 *   then halts in a watchdog-safe loop.
 * =========================================================================== */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "stm32h7xx_hal.h"
#include "gw_buttons.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "gittag.h"
#include "drc_probe.h"

#include "common.h"             /* common_emu_*_dwt_cycles, wdog_refresh */
#include "odroid_overlay.h"
#include "odroid_display.h"
#include "odroid_colors.h"

/* No other boot hook combines these two -- B_PAUSE is not read at boot at
 * all elsewhere, B_B only in-game. Deliberately disjoint from RC_COMBO
 * (B_GAME|B_TIME) and from every single-key check gw_boot_rescue.c makes
 * (B_POWER, B_TIME, B_A, B_GAME). */
#define DRC_COMBO       (B_B | B_PAUSE)

#define DRC_PROBE_PATH  "/32x_drc_probe.txt"
#define DRC_ITERS       200000u   /* loop-body iterations per measurement */

/* ---------------------------------------------------------------------- *
 * The probe body: ONE hand-authored Thumb-2 routine, assembled once.
 *
 *   r0 (in)  = iteration count (consumed to zero)
 *   r1 (in)  = seed
 *   r0 (out) = final accumulator (AAPCS return register)
 *
 * 15 instructions/iteration: a dependent add/eor chain over r2..r7 closed
 * by `subs`+`bne` -- dense ALU code with one backward branch, the shape of
 * a DRC-emitted basic block, not a 2-instruction NOP loop a fetch buffer
 * could hide behind. `.thumb` sets the assembler's instruction-encoding
 * mode; it does NOT tag the label with the ELF Thumb bit the way
 * `.thumb_func` would -- that keeps drc_probe_body_end - drc_probe_body_start
 * an exact byte count. The Thumb bit is added by hand at every call site
 * below (mandatory for BX/BLX on ARMv7-M regardless).
 * ---------------------------------------------------------------------- */
__asm__ (
    ".pushsection .text.drc_probe_body, \"ax\", %progbits\n"
    ".syntax unified\n"   /* required for the 3-operand/shifted-register Thumb-2
                            * encodings below (ADD/EOR.W) -- see memcpy-armv7m.s
                            * for the only other precedent in this tree */
    ".thumb\n"
    ".align 2\n"
    ".global drc_probe_body_start\n"
    "drc_probe_body_start:\n"
    "    push {r4-r7, lr}\n"
    "1:\n"
    "    subs r0, r0, #1\n"
    "    eor  r2, r1, r0\n"
    "    add  r3, r2, r1, ror #3\n"
    "    eor  r4, r3, r2\n"
    "    add  r5, r4, r3, ror #5\n"
    "    eor  r6, r5, r4\n"
    "    add  r7, r6, r5, ror #7\n"
    "    eor  r2, r7, r6\n"
    "    add  r3, r2, r7, ror #9\n"
    "    eor  r4, r3, r2\n"
    "    add  r5, r4, r3, ror #11\n"
    "    eor  r6, r5, r4\n"
    "    add  r7, r6, r5, ror #13\n"
    "    eor  r1, r7, r6\n"
    "    bne  1b\n"
    "    mov  r0, r1\n"
    "    pop  {r4-r7, pc}\n"
    ".global drc_probe_body_end\n"
    "drc_probe_body_end:\n"
    ".popsection\n"
);

extern uint8_t drc_probe_body_start[];
extern uint8_t drc_probe_body_end[];

typedef uint32_t (*drc_probe_fn)(uint32_t iters, uint32_t seed);

static volatile uint32_t g_sink;

/* ---------------------------------------------------------------------- */

static uint16_t dp_y;
static void dp_line(uint16_t color, const char *fmt, ...)
{
    char buf[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("drc: %s\n", buf);
    if (dp_y < GW_LCD_HEIGHT)
        dp_y = (uint16_t)odroid_overlay_draw_text(0, dp_y, GW_LCD_WIDTH, buf, color, C_BLACK);
}

/* Append one line (or block) to the report file. "ab" + immediate fclose,
 * same pattern md32x_profile.c's boot-time diag writes use -- so a crash on
 * the very next region still leaves this one durable on the SD card. */
static void dp_report(const char *fmt, ...)
{
    char buf[400];   /* the "verdict" block below is the longest message */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    FILE *f = fopen(DRC_PROBE_PATH, "ab");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}

/* Measure one placement: COLD (caches invalidated right before the call)
 * then WARM (called again immediately, region now cache-hot for the
 * cacheable placements). Returns via out-params; the accumulator result
 * (same for cold/warm chain) is returned for the cross-placement
 * correctness check. */
static uint32_t dp_measure(drc_probe_fn fn, uint32_t *cold_cyc, uint32_t *warm_cyc)
{
    uint32_t r_cold, r_warm;

    SCB_InvalidateICache();
    SCB_InvalidateDCache();
    __DSB(); __ISB();
    common_emu_clear_dwt_cycles();
    r_cold = fn(DRC_ITERS, 0x12345678u);
    *cold_cyc = common_emu_get_dwt_cycles() / DRC_ITERS;

    common_emu_clear_dwt_cycles();
    r_warm = fn(DRC_ITERS, r_cold);
    *warm_cyc = common_emu_get_dwt_cycles() / DRC_ITERS;

    g_sink ^= r_warm;
    return r_cold;    /* deterministic given the fixed seed above */
}

/* Copy the probe body into `dest`, flush it out of D-cache and invalidate
 * I-cache so the copy (not stale cache content) is what executes, then
 * measure it. `dest` must be word-aligned and >= body_size bytes. */
static uint32_t dp_measure_copy(uint8_t *dest, uint32_t body_size,
                                 uint32_t *cold_cyc, uint32_t *warm_cyc)
{
    memcpy(dest, drc_probe_body_start, body_size);
    SCB_CleanDCache_by_Addr((uint32_t *)dest, (int32_t)body_size);
    SCB_InvalidateICache();
    __DSB(); __ISB();

    drc_probe_fn fn = (drc_probe_fn)((uintptr_t)dest | 1u);   /* Thumb bit: mandatory on ARMv7-M */
    return dp_measure(fn, cold_cyc, warm_cyc);
}

/* ===========================================================================
 *  Main probe entry
 * =========================================================================== */
void drc_probe_run_if_requested(uint32_t boot_buttons)
{
    uint32_t body_size;
    uint32_t itcm_cold = 0, itcm_warm = 0, dtcm_cold = 0, dtcm_warm = 0;
    uint32_t ram_cold = 0, ram_warm = 0, flash_cold = 0, flash_warm = 0;
    uint32_t ram_r, flash_r;
    uint8_t *itcm_dest, *ram_dest;
    void *dtcm_dest;
    /* Correctness cross-check: every placement runs the SAME deterministic
     * arithmetic (fixed seed), so every result must be bit-identical. `ok`
     * starts true and only flips false on an actual mismatch; `have_ref`
     * guards against comparing against an uninitialized reference if the
     * first (ITCM) allocation ever fails. */
    uint32_t correctness_ref = 0;
    int have_ref = 0;
    int ok = 1;

    if ((boot_buttons & DRC_COMBO) != DRC_COMBO) return;   /* inert otherwise */

    body_size = (uint32_t)(drc_probe_body_end - drc_probe_body_start);

    lcd_backlight_off();
    odroid_display_set_backlight(ODROID_BACKLIGHT_LEVEL6);
    common_emu_enable_dwt_cycles();

    lcd_sync(); lcd_reset_active_buffer(); dp_y = 0;
    dp_line(C_GREEN, "DRC PROBE %s", GIT_TAG);
    dp_line(C_WHITE, "body=%uB iters=%u", (unsigned)body_size, (unsigned)DRC_ITERS);
    lcd_sync();

    /* Fresh report file -- header only, then every region below appends. */
    {
        FILE *f = fopen(DRC_PROBE_PATH, "wb");
        if (f) {
            fprintf(f, "=== DRC instruction-fetch probe (%s) ===\n", GIT_TAG);
            fprintf(f, "body_size=%u bytes  iters/measurement=%u\n",
                    (unsigned)body_size, (unsigned)DRC_ITERS);
            fprintf(f, "each region: COLD (icache+dcache invalidated before call), "
                       "WARM (called again immediately after)\n");
            fprintf(f, "values are DWT cycles / loop-body iteration (15 instructions/iter)\n\n");
            fclose(f);
        }
    }
    wdog_refresh();

    /* ---- ITCM (0x00000000, zero-wait, the known-good baseline) -------- */
    itcm_dest = (uint8_t *)itc_malloc(body_size);
    if (itcm_dest == (uint8_t *)0xffffffffu) {
        dp_line(C_RED, "ITCM alloc FAIL");
        dp_report("ITCM: alloc FAILED (body_size=%u too big for free ITCM?)\n", (unsigned)body_size);
    } else {
        uint32_t itcm_r = dp_measure_copy(itcm_dest, body_size, &itcm_cold, &itcm_warm);
        dp_line(C_WHITE, "ITCM  cold=%u warm=%u", (unsigned)itcm_cold, (unsigned)itcm_warm);
        dp_report("ITCM  (0x%08x): cold=%u warm=%u cyc/iter\n",
                   (unsigned)(uintptr_t)itcm_dest, (unsigned)itcm_cold, (unsigned)itcm_warm);
        correctness_ref = itcm_r;
        have_ref = 1;
    }
    lcd_sync();
    wdog_refresh();

    /* ---- DTCM (0x20000000, D-Code bus -- the question this probe answers) */
    dtcm_dest = malloc(body_size);
    if (dtcm_dest == NULL) {
        dp_line(C_RED, "DTCM alloc FAIL");
        dp_report("DTCM: alloc FAILED (body_size=%u, heap exhausted?)\n", (unsigned)body_size);
    } else {
        uint32_t dtcm_r;
        dp_report("DTCM: about to call -- if the report ends here, DTCM code "
                   "fetch faulted this device (see file comment: crash IS data)\n");
        dtcm_r = dp_measure_copy((uint8_t *)dtcm_dest, body_size, &dtcm_cold, &dtcm_warm);
        dp_line(C_WHITE, "DTCM  cold=%u warm=%u", (unsigned)dtcm_cold, (unsigned)dtcm_warm);
        dp_report("DTCM  (0x%08x): cold=%u warm=%u cyc/iter\n",
                   (unsigned)(uintptr_t)dtcm_dest, (unsigned)dtcm_cold, (unsigned)dtcm_warm);
        if (!have_ref) { correctness_ref = dtcm_r; have_ref = 1; }
        else if (dtcm_r != correctness_ref) ok = 0;
    }
    lcd_sync();
    wdog_refresh();

    /* ---- RAM_EMU (0x24000000+, AXI SRAM, I-cached; nothing loaded yet) - */
    ram_dest = (uint8_t *)&__RAM_EMU_START__;
    ram_r = dp_measure_copy(ram_dest, body_size, &ram_cold, &ram_warm);
    dp_line(C_WHITE, "RAM_EMU cold=%u warm=%u", (unsigned)ram_cold, (unsigned)ram_warm);
    dp_report("RAM_EMU (0x%08x): cold=%u warm=%u cyc/iter\n",
               (unsigned)(uintptr_t)ram_dest, (unsigned)ram_cold, (unsigned)ram_warm);
    if (!have_ref) { correctness_ref = ram_r; have_ref = 1; }
    else if (ram_r != correctness_ref) ok = 0;
    lcd_sync();
    wdog_refresh();

    /* ---- INTFLASH (as-linked, this TU's own .text -> internal flash XIP,
     * bank set by INTFLASH_BANK) -- free bonus reference point, no copy. */
    {
        drc_probe_fn flash_fn = (drc_probe_fn)((uintptr_t)drc_probe_body_start | 1u);
        flash_r = dp_measure(flash_fn, &flash_cold, &flash_warm);
        dp_line(C_WHITE, "FLASH cold=%u warm=%u", (unsigned)flash_cold, (unsigned)flash_warm);
        dp_report("INTFLASH (0x%08x, as-linked): cold=%u warm=%u cyc/iter\n",
                   (unsigned)(uintptr_t)drc_probe_body_start, (unsigned)flash_cold, (unsigned)flash_warm);
        if (!have_ref) { correctness_ref = flash_r; have_ref = 1; }
        else if (flash_r != correctness_ref) ok = 0;
    }
    lcd_sync();
    wdog_refresh();

    /* ---- summary + ratios (DTCM is the number this whole probe exists for) */
    {
        char rl[16];
        if (dtcm_cold) snprintf(rl, sizeof(rl), "%u.%ux",
                                 (unsigned)(dtcm_cold / (itcm_cold ? itcm_cold : 1)),
                                 (unsigned)((dtcm_cold * 10 / (itcm_cold ? itcm_cold : 1)) % 10));
        else snprintf(rl, sizeof(rl), "n/a");

        dp_line(ok ? C_GREEN : C_RED, "correctness=%s", ok ? "PASS" : "FAIL");
        dp_line(C_YELLOW, "DTCM/ITCM ratio=%s", rl);

        dp_report("\ncorrectness cross-check (all four placements, same deterministic "
                   "arithmetic, must match): %s (ref=0x%08x)\n", ok ? "PASS" : "FAIL",
                   (unsigned)correctness_ref);
        dp_report("ratios (cold cyc/iter):\n");
        dp_report("  DTCM/ITCM    = %u.%02u\n",
                   (unsigned)(dtcm_cold / (itcm_cold ? itcm_cold : 1)),
                   (unsigned)((dtcm_cold * 100 / (itcm_cold ? itcm_cold : 1)) % 100));
        dp_report("  RAM_EMU/ITCM = %u.%02u\n",
                   (unsigned)(ram_cold / (itcm_cold ? itcm_cold : 1)),
                   (unsigned)((ram_cold * 100 / (itcm_cold ? itcm_cold : 1)) % 100));
        dp_report("  FLASH/ITCM   = %u.%02u\n",
                   (unsigned)(flash_cold / (itcm_cold ? itcm_cold : 1)),
                   (unsigned)((flash_cold * 100 / (itcm_cold ? itcm_cold : 1)) % 100));
        dp_report("verdict: if DTCM cold cyc/iter is within ~1.5x of ITCM, a DTCM code "
                   "cache is viable and the ARM32->Thumb-2 DRC backend is worth writing. "
                   "If it is many x slower (or this file ends before the DTCM line above), "
                   "the 73KB-DTCM-cache design does not pay and must change first.\n");
    }

    printf("drc: halt (probe done)\n");
    for (;;) { wdog_refresh(); HAL_Delay(50); }
}
