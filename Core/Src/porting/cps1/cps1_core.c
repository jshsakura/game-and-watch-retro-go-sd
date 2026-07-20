#include "cps1_core.h"

/*
 * TODO(cps1): CPS1_ENGINE_INTERPRETER should become the real 68000+Z80
 * interpreter and PPU renderer; CPS1_ENGINE_RECOMPILER the static
 * recompiler's translated output (docs/CPS1_ULTIMATE_PORTING_PLAN.md
 * technique 1). Until either exists, both engines run this identical stub,
 * so a passing interpreter-vs-recompiler diff only proves the harness
 * compares correctly -- it proves nothing about emulation correctness.
 */

typedef struct {
    uint16_t fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
    uint32_t frame;
} cps1_engine_state_t;

static cps1_engine_state_t s_engine[CPS1_ENGINE_COUNT];

void cps1_core_reset(cps1_engine_kind_t engine)
{
    cps1_engine_state_t *e = &s_engine[engine];
    e->frame = 0;
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++)
        e->fb[i] = 0;
}

void cps1_core_run_frame(cps1_engine_kind_t engine)
{
    cps1_engine_state_t *e = &s_engine[engine];
    e->frame++;

    /* Deterministic synthetic pattern standing in for a rendered frame: a
     * diagonal RGB565 gradient that shifts one step per frame. */
    for (int y = 0; y < CPS1_FB_HEIGHT; y++) {
        for (int x = 0; x < CPS1_FB_WIDTH; x++) {
            uint32_t v = (uint32_t)x + (uint32_t)y + e->frame;
            uint16_t r5 = (uint16_t)(v & 0x1Fu);
            uint16_t g6 = (uint16_t)((v * 3u) & 0x3Fu);
            uint16_t b5 = (uint16_t)((v * 7u) & 0x1Fu);
            e->fb[y * CPS1_FB_WIDTH + x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

const uint16_t *cps1_core_get_framebuffer(cps1_engine_kind_t engine)
{
    return s_engine[engine].fb;
}

uint32_t cps1_core_checksum(cps1_engine_kind_t engine)
{
    const uint16_t *fb = s_engine[engine].fb;
    uint32_t h = 2166136261u;
    for (int i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        h ^= fb[i];
        h *= 16777619u;
    }
    return h;
}
