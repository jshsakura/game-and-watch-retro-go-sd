#pragma once
/*
 * CPS-1 core: shared by every CPS-1 harness (linux/cps1, the M7 QEMU rig,
 * and eventually the device .overlay_cps1). Freestanding on purpose -- only
 * <stdint.h>, no malloc -- so the identical file compiles unmodified under
 * glibc and under the bare-metal ARMv7-M rig.
 *
 * STUB ONLY right now: reset/run_frame synthesize a deterministic test
 * pattern instead of emulating the 68000/Z80/PPU/sound hardware. This lets
 * the harness plumbing (frame loop, checksum, interpreter-vs-recompiler
 * diff) be proven before any real CPU/video/sound code exists. See
 * docs/CPS1_FEASIBILITY.md and docs/CPS1_SENIOR_TRICKS_ANALYSIS.md.
 */
#include <stdint.h>

#define CPS1_FB_WIDTH  384
#define CPS1_FB_HEIGHT 224

typedef enum {
    CPS1_ENGINE_INTERPRETER = 0,
    CPS1_ENGINE_RECOMPILER  = 1,
    CPS1_ENGINE_COUNT       = 2,
} cps1_engine_kind_t;

void cps1_core_reset(cps1_engine_kind_t engine);
void cps1_core_run_frame(cps1_engine_kind_t engine);
const uint16_t *cps1_core_get_framebuffer(cps1_engine_kind_t engine);
uint32_t cps1_core_checksum(cps1_engine_kind_t engine);
