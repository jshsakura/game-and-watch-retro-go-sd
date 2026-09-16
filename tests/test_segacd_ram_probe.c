/* Host harness for the Sega CD RAM probe (gates 4+5 code path).
 *
 * The REAL Core/Src/segacd_ram_probe.c is included whole below and run with
 * the device combo latched, against host memory through the stubs in
 * tests/segacd_stubs/. What this proves BEFORE the single device session:
 * the trigger gate, the gate-4 malloc sequence, the 14-region pattern sweep,
 * the report format and the halt handshake. What it deliberately does NOT
 * prove: physical AXI/AHB/DTCM/ITCM addresses and the newlib-nano heap —
 * that is the device run (docs/SEGACD_REASSESSMENT_2026-09-14.md gates 4/5;
 * the project once shipped a "GO" on host-harness output alone and the
 * screen never came up — stale record, see the reassessment).
 *
 * RED-verified: -DSCD_RED_TEST (stubs.c) flips a passing region line to
 * "RW FAIL" and this harness exits nonzero.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "gw_buttons.h"

/* SEGACD_RAM_PROBE comes from the compile line (see tests/run.sh). */
#include "../Core/Src/segacd_ram_probe.c"

/* from stubs.c */
extern char scd_lines[32][96];
extern int scd_nlines;
extern int scd_overflow;
extern jmp_buf scd_halt;

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } \
} while (0)

static const char *cs_of(const char *line)
{
    return strstr(line, "cs=") ? strstr(line, "cs=") : "";
}

int main(void)
{
    printf("=== segacd_ram_probe: host harness (gates 4+5 code path) ===\n");

    /* 1. Inert without the combo: no lines, no halt. */
    scd_nlines = 0;
    if (setjmp(scd_halt) == 0)
        segacd_ram_probe_run_if_requested(0);
    CHECK(scd_nlines == 0, "probe must be inert without GAME+TIME");

    /* 2. Full run: returns only through the HAL_Delay longjmp. */
    scd_nlines = 0;
    if (setjmp(scd_halt) == 0)
        segacd_ram_probe_run_if_requested(B_GAME | B_TIME);
    CHECK(scd_overflow == 0, "LCD line recorder did not overflow");

    /* header + G4 + 14 regions + summary = 17 lines */
    CHECK(scd_nlines == 17, "exactly 17 report lines (got %d)", scd_nlines);
    if (scd_nlines >= 1)
        CHECK(strstr(scd_lines[0], "SCD PROBE") != NULL, "header line");
    if (scd_nlines >= 2) {
        CHECK(strstr(scd_lines[1], "G4") != NULL, "gate-4 line present");
        CHECK(strstr(scd_lines[1], "margin claim ok") != NULL,
              "gate-4 margin claimed cleanly");
        CHECK(strstr(scd_lines[1], "page7@0x") != NULL,
              "gate-4 reports page 7 address");
    }
    for (int i = 2; i < 16 && i < scd_nlines; i++) {
        CHECK(strstr(scd_lines[i], "FAIL") == NULL,
              "region line %d clean", i);
        CHECK(strstr(scd_lines[i], "cs=") != NULL,
              "region line %d carries a checksum", i);
    }
    if (scd_nlines >= 17) {
        CHECK(strstr(scd_lines[16], "G4 PASS") != NULL, "summary: G4 PASS");
        CHECK(strstr(scd_lines[16], "G5 14/14") != NULL,
              "summary: all 14 regions passed");
    }

    /* Checksums must discriminate: two same-size PRG pages (different seed
     * and address) must not hash identically. */
    if (scd_nlines >= 4)
        CHECK(strcmp(cs_of(scd_lines[2]), cs_of(scd_lines[3])) != 0,
              "prg0 and prg1 checksums differ");

    /* The eight PRG backing stores must not overlap (host objects here; the
     * device report proves the same contract with real bank addresses). */
    static unsigned char *pages[8] = {
        probe_segacd_prg_page0, probe_segacd_prg_page1,
        probe_segacd_prg_page2, probe_segacd_prg_page3,
        probe_segacd_prg_page4, probe_segacd_prg_page5,
        probe_segacd_prg_page6, NULL, /* page 7: DTCM heap at runtime */
    };
    for (int i = 0; i < 7; i++)
        for (int j = i + 1; j < 7; j++) {
            long d = (long)(intptr_t)pages[i] - (long)(intptr_t)pages[j];
            if (d < 0) d = -d;
            CHECK(d >= 65536, "PRG pages %d,%d do not overlap", i, j);
        }

    /* The probe frees the margin block but deliberately keeps page 7
     * claimed; the host heap must still serve a fresh margin-sized block. */
    void *m = malloc(24696 - 16);
    CHECK(m != NULL, "margin block still allocatable after probe (freed)");
    free(m);

    if (failures) {
        printf("=== %d check(s) failed ===\n", failures);
        return 1;
    }
    printf("=== segacd_ram_probe host harness: all checks passed ===\n");
    return 0;
}
