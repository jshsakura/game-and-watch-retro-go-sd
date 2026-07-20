/*
 * Build-test for the 68000 -> sound-command-latch -> HLE mixer wiring in
 * cps1_core.c: a real 68000 program (MOVEA/MOVEQ/MOVE.W) writes a command
 * byte through the SAME cps1_bus_write16 dispatcher the engine slots use,
 * and this checks the sound HLE actually reacted -- not just that the bus
 * write didn't crash.
 *
 *   ./build/cps1-sound-bus-selftest
 */
#include <stdio.h>

#include "cps1_core.h"

int main(void)
{
    int ok = cps1_core_selftest_sound_bus();

    printf("[cps1-sound-bus-selftest] tone channel 0 active=%d\n",
           cps1_core_sound_tone_active(0));

    if (!ok) {
        fprintf(stderr, "[cps1-sound-bus-selftest] FAIL\n");
        return 1;
    }
    printf("[cps1-sound-bus-selftest] OK\n");
    return 0;
}
