/*
 * Build-test for Phase 9 (docs/CPS1_MAME_ALIGNMENT.md sections 3/4/5/9):
 * indirect gfxram addressing through the CPS-A base registers, and the
 * one-frame OBJ RAM delay. Order matters here -- the relocation tests
 * permanently move OBJ_BASE/PALETTE_BASE away from their defaults within
 * this process, so obj_delay (which assumes defaults) must run first.
 *
 *   ./build/cps1-phase9-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int failures = 0;

    int ok1 = cps1_core_selftest_obj_delay();
    printf("[cps1-phase9-selftest] obj_delay: %s\n", ok1 ? "OK" : "FAIL");
    if (!ok1) failures++;

    int ok2 = cps1_core_selftest_obj_relocation();
    printf("[cps1-phase9-selftest] obj_relocation: %s\n", ok2 ? "OK" : "FAIL");
    if (!ok2) failures++;

    int ok3 = cps1_core_selftest_palette_relocation();
    printf("[cps1-phase9-selftest] palette_relocation: %s\n", ok3 ? "OK" : "FAIL");
    if (!ok3) failures++;

    if (failures) {
        fprintf(stderr, "[cps1-phase9-selftest] FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("[cps1-phase9-selftest] OK\n");
    return 0;
}
