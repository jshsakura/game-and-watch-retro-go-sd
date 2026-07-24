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

/*
 * Optimization phase (post-Phase 12: real LTDC-era cost measured OVER the
 * 60fps budget, docs/CPS1_MAME_ALIGNMENT.md section 9): section-attribute
 * hints for placing the hottest code/data in the STM32H7's zero-wait-state
 * ITCM/DTCM regions, matching how other cores in this repo (e.g. PCE's
 * `.overlay_pce_itc`) get their hot code into ITCM.
 *
 * ================================ HONESTY NOTE ================================
 * cps1 has no real linked `.overlay_cps1`/ITCM/DTCM section list yet
 * -- these attributes currently just rename the ELF section a
 * function/object lands in; on both the Linux host build and the QEMU M7
 * rig's linker script (tools/m7_qemu_rig/mps2_an500.ld, which explicitly
 * has ONE "CODE" region and no ITCM/DTCM distinction, no wait-state model
 * at all -- see that file's own header comment) they are functionally
 * INERT: the code executes at the identical instruction count either way.
 * Real benefit (avoiding flash wait-states / cache misses on every fetch)
 * requires (a) a real `.itcm_cps1`-style output section in the device
 * linker script, (b) a boot-time copy from flash into ITCM (ITCM is
 * volatile RAM, not XIP -- see PCE's `run_internal_emu` memcpy), and (c)
 * real hardware DWT/cycle profiling to measure it, none of which exist for
 * cps1 yet. Do not expect (and this project will not claim) any QEMU
 * insn/frame delta from these attributes alone -- see this phase's commit
 * message for the before/after numbers that prove that expectation.
 * ================================================================================
 */
/* ITCM/DTCM placement is DISABLED. The linker section `.cps1_itcm_text`
 * (STM32H7B0VBTx_SDCARD.ld) puts these functions at an ITCM VMA with a flash
 * LMA, but the runtime memcpy(LMA->VMA) that PCE's overlay does was never
 * wired for cps1 -- it was section-placement scaffolding from when cps1 was an
 * isolated harness. Now that cps1 is a linked, running system, the attribute
 * placed the blitter (cps1_blit8x8_indexed etc.) at an ITCM address that is
 * NEVER populated, so the first blit call on device jumped into uninitialised
 * ITCM = a wild PC BusFault, right where /cps1_diag.txt stopped ("bg layer 2",
 * the first blit). The attribute is documented to give no measurable speedup on
 * its own, so drop it: the code then lives in .overlay_cps1 (RAM_EMU), which IS
 * copied in when the core loads, and runs from a valid address. Re-enable only
 * together with a real LMA->VMA copy at cps1 load. */
#define CPS1_ITCM_TEXT
#define CPS1_DTCM_BSS

#define CPS1_FB_WIDTH  384
#define CPS1_FB_HEIGHT 224

/* First-frame breadcrumb log to /cps1_diag.txt (defined in main_cps1.c). Callable
 * from the render TUs so a crash inside a layer/blit leaves the exact step on the
 * SD. Sealed after frame 0, so it is free during steady play. */
void cps1_diag(const char *fmt, ...);

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

/* Device-realistic pipeline (cheat 8): CPU + SCROLL3 + sprites only, into
 * ONE buffer (cps1_core_get_framebuffer). This is what becomes LTDC
 * hardware layer 1 (the "top" layer) on real hardware -- see
 * cps1_core_render_ltdc_bottom below for the OTHER layer. The compositor
 * BLEND between the two layers is what's actually free on real hardware
 * (LTDC's own alpha/scanout hardware) -- SCROLL3/sprite RENDERING is
 * still real CPU work, same as always. Profile THIS (plus the bottom
 * layer below) against the 60fps budget. */
void cps1_core_run_frame_device_cost(cps1_engine_kind_t engine);

/*
 * Phase 12 (docs/CPS1_MAME_ALIGNMENT.md section 9, LTDC dual-layer
 * binding): renders SCROLL1+SCROLL2 combined (plain overwrite, no
 * priority-group logic -- BG-vs-BG order is fixed regardless of masks)
 * into a SEPARATE buffer from cps1_core_run_frame_device_cost()'s output.
 * STM32H7's LTDC has exactly 2 hardware layers: this buffer becomes layer
 * 0 (bottom), cps1_core_get_framebuffer(engine)'s becomes layer 1 (top,
 * with LTDC hardware alpha-blending them at scanout for zero CPU cost).
 * NOTE: the device path does not do this. main_cps1.c renders straight
 * into lcd_get_active_buffer() like every other core; the two-layer LTDC
 * plan was never wired and its code is gone (see the 0722 cleanup commit
 * if it is ever wanted back). Not per-engine (BG state is shared, like the rest of
 * this file) -- call once per frame alongside run_frame_device_cost(),
 * not instead of it.
 */
void cps1_core_render_ltdc_bottom(void);
const uint16_t *cps1_core_get_ltdc_bottom_buffer(void);

/* Real LCD panel is 320x240 (Core/Inc/gw_lcd.h), narrower than CPS-1's
 * 384x224 native resolution -- CPS1_PANEL_WIDTH/HEIGHT below is the
 * center-cropped window LTDC actually scans out; see cps1_core.c's
 * comment on cps1_core_crop_to_panel for why this specific choice (crop,
 * not scale) is a placeholder, not a validated design decision. `dst`
 * must be CPS1_PANEL_WIDTH*CPS1_PANEL_HEIGHT uint16_t. */
#define CPS1_PANEL_WIDTH  320
#define CPS1_PANEL_HEIGHT 224
void cps1_core_crop_to_panel(const uint16_t *src, uint16_t *dst);

/* Proves the crop takes exactly the centered CPS1_PANEL_WIDTH-wide
 * window of each row, not an off-by-one/uncentered slice. */
int cps1_core_selftest_crop_to_panel(void);

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
/* out_attr is uint16_t, not uint8_t -- Phase 10 corrected the OAM attr
 * word to its real width (color 0-31 + block-size nibbles need more than
 * 8 bits, docs/CPS1_MAME_ALIGNMENT.md section 5). */
int cps1_core_oam_peek(uint32_t index, int16_t *out_x, int16_t *out_y,
                        uint16_t *out_tile, uint16_t *out_attr);
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
 * 1=SCROLL2, 2=SCROLL3. Returns the raw code+attr words (Phase 10 -- no
 * more separate tile/palette/enabled fields; decode attr's color/flip/
 * priority bits via cps1_bg_attr_* in cps1_bg.h if needed). */
int cps1_core_bg_cell_peek(unsigned layer, uint32_t index, uint16_t *out_code,
                            uint16_t *out_attr);

/* Hand-assembled program writes a tilemap cell (code + attr word) through
 * MOVEA/MOVE.W to the real bus dispatcher, then checks SCROLL1's cell
 * (0,0) actually changed -- proving the 68000 can reach the BG tilemap,
 * not just that cps1_bg.c compiles. */
int cps1_core_selftest_bg_bus(void);

/*
 * Phase 9 (docs/CPS1_MAME_ALIGNMENT.md sections 3/4/5/9): OBJ/SCROLL1/2/3/
 * PALETTE are no longer separate fixed bus regions -- they're views into
 * one shared 192KB gfxram pool (0x900000-0x92FFFF) at offsets the CPS-A
 * registers (0x800100-0x80013F) specify, exactly like real hardware's
 * cps1_base(). Moving a base register genuinely relocates where
 * subsequent writes land.
 */

/* cps1_core_oam_peek (declared above) reads back what the CPU is
 * CURRENTLY writing (immediate, not delayed) -- see
 * cps1_core_buffered_oam_peek below for what's actually rendered.
 *
 * OBJ RAM is delayed one whole frame on real hardware
 * (docs/CPS1_MAME_ALIGNMENT.md section 5, cps1_objram_latch): this frame's
 * CPU writes (visible via cps1_core_oam_peek above) only show up in the
 * buffer cps1_ppu_render actually reads starting NEXT frame. Same
 * primitive-out-params shape as cps1_core_oam_peek, reading the latched
 * copy instead of the live one. */
int cps1_core_buffered_oam_peek(uint32_t index, int16_t *out_x, int16_t *out_y,
                                 uint16_t *out_tile, uint16_t *out_attr);

/* Hand-assembled program writes sprite 0's Y field through the bus,
 * confirms it landed in the live table (cps1_core_oam_peek) but NOT yet
 * in the buffered/rendered one; runs one cps1_core_run_frame(); confirms
 * it's NOW in the buffered table; writes a second, different Y; confirms
 * the buffer still shows the FIRST value (not yet re-latched); runs a
 * second frame; confirms the buffer now shows the second value. Proves
 * the one-frame delay persists across multiple writes, not just once. */
int cps1_core_selftest_obj_delay(void);

/* Hand-assembled program changes the OBJ_BASE CPS-A register to a new
 * gfxram offset, writes sprite 0's Y field at the NEWLY-resolved absolute
 * address, then writes a different value at the OLD default OBJ address
 * -- and checks the write landed at the new location and the old
 * location's write did NOT alias back onto sprite 0. Proves base-register
 * relocation actually moves where writes land, not just that writes work
 * at a fixed address. */
int cps1_core_selftest_obj_relocation(void);

/* Same proof for PALETTE_BASE: relocates it, writes a raw palette word at
 * the new address (checked via cps1_palette_build), then writes a
 * different raw word at the OLD default palette address and checks it did
 * NOT change the color that was just set at the new location. */
int cps1_core_selftest_palette_relocation(void);

/*
 * Phase 10 (docs/CPS1_MAME_ALIGNMENT.md sections 4/5/6/9): sprite/BG
 * field-layout + priority. cps1_oam_entry_t is now X,Y,tile,attr with the
 * real attr bit layout (multi-tile block sprites); cps1_bg_cell_t is now
 * literal code+attr (see cps1_bg.h for the decode accessors and the new
 * cps1_compositor_blend_priority that replaces the old unconditional
 * bottom<middle<top blend). The pure rendering-logic proofs (multi-tile
 * block+flip sprites, BG flip, per-layer palette offset, bit-swizzle
 * tilemap addressing, priority punch-through) live in linux/cps1/
 * ppu_selftest.c and bg_selftest.c against cps1_ppu.c/cps1_bg.c directly --
 * only the NEW bus-reachability proof needs a cps1_core.c entry point.
 */

/* Read-only inspection of a CPS-B priority-bitmask register (0-3) --
 * see cps1_priority_masks_t in cps1_bg.h. */
uint16_t cps1_core_priority_mask_peek(unsigned group);

/* Hand-assembled program writes a raw value to CPS-B priority-mask
 * register 0 through MOVEA/MOVE.W to the real bus dispatcher, then checks
 * it landed in s_priority_masks.masks[0] -- proving the 68000 can reach
 * the priority masks cps1_compositor_blend_priority reads, not just that
 * the masks work when poked directly in a host test. */
int cps1_core_selftest_priority_bus(void);

/* Phase 12: proves cps1_core_render_ltdc_bottom() combines SCROLL1+SCROLL2
 * (plain overwrite, SCROLL2 on top) and is NOT affected by SCROLL3 or
 * sprites -- see cps1_core.c for the exact pixel checks. */
int cps1_core_selftest_ltdc_bottom(void);
