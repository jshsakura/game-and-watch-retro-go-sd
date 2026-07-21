/*
 * Build-test for the 68000<->VDP bus wired in cps1_core.c: a real 68000
 * program (MOVEA/MOVEQ/MOVE.W) runs through the SAME cps1_bus_read16/
 * write16 dispatcher the engine slots use, and this checks it actually
 * moved a sprite and recolored a palette entry -- not just that memory
 * access didn't crash.
 *
 *   ./build/cps1-vdp-bus-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int ok = cps1_core_selftest_vdp_bus();

    int16_t x = 0, y = 0;
    uint16_t tile = 0;
    uint16_t attr = 0;
    cps1_core_oam_peek(0, &x, &y, &tile, &attr);

    printf("[cps1-vdp-bus-selftest] wram[0]=%u sprite0.y=%d "
           "palette[1][2]=0x%04x (raw 21 converted via cps1_palette_build)\n",
           cps1_core_wram_peek16(0), y, cps1_core_palette_peek(1, 2));

    if (!ok) {
        fprintf(stderr, "[cps1-vdp-bus-selftest] FAIL\n");
        return 1;
    }
    printf("[cps1-vdp-bus-selftest] OK\n");
    return 0;
}
