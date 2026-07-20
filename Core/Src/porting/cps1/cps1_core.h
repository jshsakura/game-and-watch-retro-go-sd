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

/* Full pipeline for correctness/visual verification: CPU + all 3 BG layers
 * + sprites + the host-only compositor stand-in for LTDC. Use this for
 * checksums/PPM dumps, NOT for fps profiling -- see the device-cost
 * variant below for what real hardware actually pays per frame. */
void cps1_core_run_frame(cps1_engine_kind_t engine);

/* Device-realistic pipeline (cheat 8): CPU + SCROLL3 + sprites only.
 * SCROLL1/SCROLL2 render and the compositor blend are skipped -- on real
 * hardware those become LTDC hardware layers/scanout blending and cost
 * the CPU nothing. Profile THIS against the 60fps budget. */
void cps1_core_run_frame_device_cost(cps1_engine_kind_t engine);

const uint16_t *cps1_core_get_framebuffer(cps1_engine_kind_t engine);
uint32_t cps1_core_checksum(cps1_engine_kind_t engine);

/* 68000 CPU state (Core/Src/porting/cps1/cps1_cpu68k.{h,c}): runs a fixed,
 * hand-assembled test program (see cps1_core.c) until it RTS's, so the
 * fetch/decode/execute path is real and observable before any ROM-loading
 * format exists. Independent from the framebuffer hash above. */
uint32_t cps1_core_cpu_state_hash(cps1_engine_kind_t engine);
uint32_t cps1_core_cpu_illegal_count(cps1_engine_kind_t engine);

/* Read-only inspection of the 68000<->VDP bus wired in cps1_core.c (WRAM,
 * OBJ RAM / sprite table, palette RAM) -- for tests/build verification, not
 * part of the per-frame loop. Kept as primitive out-params here (rather
 * than exposing cps1_oam_entry_t) so this header stays independent of
 * cps1_ppu.h. */
uint16_t cps1_core_wram_peek16(uint32_t offset);
int cps1_core_oam_peek(uint32_t index, int16_t *out_x, int16_t *out_y,
                        uint16_t *out_tile, uint8_t *out_attr);
uint16_t cps1_core_palette_peek(unsigned bank, unsigned color);

/* Runs a small built-in program (its own standalone cps1_cpu68k_t, not an
 * engine slot's) through the REAL cps1_bus_read16/write16 dispatcher in
 * cps1_core.c -- MOVEA.L to set up a pointer, MOVE.W to write/read through
 * it -- and checks: a WRAM round-trip, an OBJ-RAM write that actually moves
 * sprite 0, and a palette-RAM write that actually recolors bank 1 color 2.
 * Returns 1 if every check passes, 0 otherwise (prints nothing -- caller
 * reports). NOTE: this mutates the shared WRAM/OAM/palette globals the
 * engine slots also render from -- it's a build-verification tool, call it
 * before running real frames, not interleaved with them. */
int cps1_core_selftest_vdp_bus(void);

/* Sound HLE (Core/Src/porting/cps1/cps1_sound_hle.{h,c}): reachable ONLY
 * through the 68000 bus write intercept at the sound-command address, the
 * same way real hardware's Z80 is only reachable through a shared latch --
 * no direct C-to-C shortcut from CPU tests into the mixer. */
int cps1_core_sound_tone_active(unsigned channel);
void cps1_core_sound_mix(int16_t *out, uint32_t count);

/* Hand-assembled program writes a "play tone" command through MOVEA/
 * MOVE.W to the real bus dispatcher, then checks the sound HLE's tone
 * channel 0 actually activated with a non-zero frequency -- proving the
 * 68000 can reach the mixer, not just that cps1_sound_hle.c compiles. */
int cps1_core_selftest_sound_bus(void);

/* Read-only inspection of a BG layer's tilemap cell -- same
 * primitive-out-params pattern as cps1_core_oam_peek, for the same reason
 * (keeps this header independent of cps1_bg.h). layer: 0=SCROLL1,
 * 1=SCROLL2, 2=SCROLL3. */
int cps1_core_bg_cell_peek(unsigned layer, uint32_t index, uint16_t *out_tile,
                            uint8_t *out_palette, uint8_t *out_enabled);

/* Hand-assembled program writes a tilemap cell (tile_index + palette/
 * enabled word) through MOVEA/MOVE.W to the real bus dispatcher, then
 * checks SCROLL1's cell 0 actually changed -- proving the 68000 can reach
 * the BG tilemap, not just that cps1_bg.c compiles. */
int cps1_core_selftest_bg_bus(void);
