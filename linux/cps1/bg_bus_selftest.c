/*
 * Build-test for the 68000 -> BG tilemap RAM wiring in cps1_core.c: a real
 * 68000 program writes a code and an attr word to SCROLL1's cell (0,0)
 * through the SAME cps1_bus_write16 dispatcher the engine slots use
 * (including the bit-swizzled cell addressing), and this checks the BG
 * layer state actually changed.
 *
 *   ./build/cps1-bg-bus-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int ok = cps1_core_selftest_bg_bus();

    uint16_t code = 0, attr = 0;
    cps1_core_bg_cell_peek(0, 0, &code, &attr);
    printf("[cps1-bg-bus-selftest] SCROLL1 cell(0,0): code=%u attr=0x%04x\n", code, attr);

    if (!ok) {
        fprintf(stderr, "[cps1-bg-bus-selftest] FAIL\n");
        return 1;
    }
    printf("[cps1-bg-bus-selftest] OK\n");
    return 0;
}
