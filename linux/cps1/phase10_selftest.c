/*
 * Build-test for the 68000 -> CPS-B priority-mask register wiring in
 * cps1_core.c (docs/CPS1_MAME_ALIGNMENT.md sections 4/9 Phase 10): a real
 * 68000 program writes to CPS-B priority-mask register 0 through the SAME
 * cps1_bus_write16 dispatcher the engine slots use, and this checks it
 * landed in the mask cps1_compositor_blend_priority actually reads. The
 * masks' EFFECT (punch-through over sprites) and the sprite/BG
 * field-layout fixes (multi-tile block+flip, per-layer palette offset,
 * bit-swizzle tilemap addressing) are proven directly against cps1_ppu.c/
 * cps1_bg.c in cps1-ppu-selftest/cps1-bg-selftest -- this binary is only
 * the bus-reachability proof, same division of labor as
 * cps1-vdp-bus-selftest/cps1-bg-bus-selftest/cps1-sound-bus-selftest.
 *
 *   ./build/cps1-priority-bus-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int ok = cps1_core_selftest_priority_bus();

    printf("[cps1-priority-bus-selftest] priority_mask[0]=0x%04x\n",
           cps1_core_priority_mask_peek(0));

    if (!ok) {
        fprintf(stderr, "[cps1-priority-bus-selftest] FAIL\n");
        return 1;
    }
    printf("[cps1-priority-bus-selftest] OK\n");
    return 0;
}
