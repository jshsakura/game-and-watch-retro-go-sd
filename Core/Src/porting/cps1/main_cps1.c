/*
 * CPS-1 device entry point.
 *
 * Modelled on linux/cps1/real_frame_harness.c, NOT on cps1_core.c. That
 * matters: cps1_core.c is the synthetic-scene testbed the early phases were
 * built on -- its own frame loop, its own fabricated OAM/tilemaps, and 2.25 MB
 * of host-only buffers (two engine copies for the diff harness, three separate
 * layer framebuffers plus metas for a software compositor, selftest scratch).
 * None of that belongs on a device with 724 KB of RAM_EMU. The harness is the
 * shape that actually renders a real game: own the bus, drive Musashi, point
 * the shipping renderer at real gfxram.
 *
 * MEMORY PLAN (measured, arm-none-eabi -O2)
 *   gfxram                 192 KB   real hardware, not negotiable
 *   work RAM                64 KB   ditto
 *   BG tilemap state        48 KB   3 layers x 4096 cells x 4 B
 *   tile cache              64 KB   CPS1_TILE_CACHE_BUDGET_BYTES override
 *   QSound shared RAM       32 KB   only 0xF18000.. is live, not the whole page
 *   palette + OAM + Musashi ~15 KB
 *   cps1 code + Musashi    ~167 KB  (Musashi's 328 KB of lookup tables are
 *                                    rodata and belong in XIP flash)
 *   ------------------------------------------------------------------
 *   ~550 KB of 724 KB
 * Framebuffers are NOT in this budget: rendering goes straight into the
 * shared LCD pool via lcd_get_active_buffer(), like every other core.
 *
 * ROM LOADING costs zero RAM. odroid_overlay_cache_file_in_flash() puts each
 * chip dump in external flash and hands back an XIP pointer, and
 * cps1_gfx_chip_byte() reads the 8 GFX chips in place -- MAME's interleave is
 * address arithmetic, so nothing is assembled and no container file exists
 * (see docs/CPS1_ROM_PIPELINE.md's retraction).
 *
 * ================================ STATUS =================================
 * NOT REACHABLE YET, deliberately. There is no APPID_CPS1, no entry in
 * rg_emulators.c, no .overlay_cps1 in the linker script and no create_sd_data
 * line. Adding an APPID grows persistent_config_t and RESETS EVERY USER'S
 * SETTINGS (CLAUDE.md), and intflash headroom is currently measured in
 * hundreds of bytes -- so that step is a deliberate decision, not something to
 * slip in with a feature. This file is written so that when the decision is
 * made the remaining work is registration, not implementation.
 * =========================================================================
 */
#include <stdint.h>
#include <string.h>

#include "odroid_system.h"
#include "common.h"
#include "gw_lcd.h"

#include "cps1_m68k.h"
#include "cps1_rom.h"
#include "cps1_ppu.h"
#include "cps1_bg.h"
#include "cps1_eeprom.h"

#define CPS1_SAMPLE_RATE     32000
#define CPS1_GFXRAM_BYTES    0x30000u
#define CPS1_GFXRAM_ADDR     0x900000u
#define CPS1_WRAM_BYTES      0x10000u
/* CPS-1's 68000 is 10 MHz; Musashi counts MUL=7 master cycles (m68kcpu.c). */
#define CPS1_FRAME_MASTER    (166666u * 7u)

static uint8_t  s_gfxram[CPS1_GFXRAM_BYTES];
static uint8_t  s_wram[CPS1_WRAM_BYTES];
/* QSound shared RAM. MAME maps only 0xF18000-0xF19FFF and 0xF1E000-0xF1FFFF
 * as live, so 32 KB from 0xF18000 covers both without carrying the whole
 * 64 KB page. */
#define CPS1_QRAM_BASE  0xF18000u
#define CPS1_QRAM_BYTES 0x8000u
static uint8_t  s_qram[CPS1_QRAM_BYTES];
static uint16_t s_regs[0xC0];              /* 0x800000-0x80017F, word indexed */

static cps1_rom_t        s_rom;
static cps1_gfx_chips_t  s_gfx_chips;
static cps1_eeprom_t     s_eeprom;
static cps1_tile_cache_t s_cache;
static cps1_palette_t    s_pal;
static cps1_bg_state_t   s_bg;
static cps1_oam_t        s_oam;

#define CPSA_REG(i) s_regs[(0x100u + (i) * 2u) >> 1]
enum { REG_OBJ = 0, REG_SCROLL1, REG_SCROLL2, REG_SCROLL3, REG_OTHER, REG_PALETTE };

/* ---------------------------------------------------------------- bus --- */

static uint16_t cps1_bus_read(uint32_t a)
{
    if (a >= CPS1_GFXRAM_ADDR && a < CPS1_GFXRAM_ADDR + CPS1_GFXRAM_BYTES) {
        uint32_t o = a - CPS1_GFXRAM_ADDR;
        return (uint16_t)((s_gfxram[o] << 8) | s_gfxram[o + 1]);
    }
    if (a >= 0xFF0000u) {
        uint32_t o = a - 0xFF0000u;
        return (uint16_t)((s_wram[o] << 8) | s_wram[o + 1]);
    }
    /* EEPROM data line. Only bit 0 is driven; see cps1_eeprom.h. */
    if ((a & ~1u) == 0xF1C006u)
        return cps1_eeprom_read_port(&s_eeprom);
    /* Player inputs are ACTIVE LOW -- 0xFFFF means nothing pressed. Handing
     * back zeroes here tells the game every button and both coin slots are
     * held from power-on, which its boot path is not written to survive. */
    if (a < 0x800020u)
        return 0xFFFFu;
    if (a >= 0x800000u && a < 0x800180u)
        return s_regs[(a - 0x800000u) >> 1];
    if (a >= CPS1_QRAM_BASE && a < CPS1_QRAM_BASE + CPS1_QRAM_BYTES) {
        uint32_t o = a - CPS1_QRAM_BASE;
        return (uint16_t)((s_qram[o] << 8) | s_qram[o + 1]);
    }
    return 0xFFFFu;
}

static void cps1_bus_write(uint32_t a, uint16_t v)
{
    if (a >= CPS1_GFXRAM_ADDR && a < CPS1_GFXRAM_ADDR + CPS1_GFXRAM_BYTES) {
        uint32_t o = a - CPS1_GFXRAM_ADDR;
        s_gfxram[o] = (uint8_t)(v >> 8); s_gfxram[o + 1] = (uint8_t)v;
        return;
    }
    if (a >= 0xFF0000u) {
        uint32_t o = a - 0xFF0000u;
        s_wram[o] = (uint8_t)(v >> 8); s_wram[o + 1] = (uint8_t)v;
        return;
    }
    if ((a & ~1u) == 0xF1C006u) { cps1_eeprom_write_port(&s_eeprom, v); return; }
    if (a >= CPS1_QRAM_BASE && a < CPS1_QRAM_BASE + CPS1_QRAM_BYTES) {
        uint32_t o = a - CPS1_QRAM_BASE;
        s_qram[o] = (uint8_t)(v >> 8); s_qram[o + 1] = (uint8_t)v;
        return;
    }
    if (a >= 0x800000u && a < 0x800180u)
        s_regs[(a - 0x800000u) >> 1] = v;
}

/* ------------------------------------------------------------- render --- */

static uint32_t cps1_resolve_base(uint16_t reg)
{
    uint32_t b = (uint32_t)reg * 256u;
    b &= ~(0x4000u - 1u);
    /* 192 KB is not a power of two, so masking corrupts the wrap; modulo. */
    return b % CPS1_GFXRAM_BYTES;
}

static uint16_t cps1_gfx_word(uint32_t off)
{
    off %= CPS1_GFXRAM_BYTES;
    return (uint16_t)((s_gfxram[off] << 8) | s_gfxram[off + 1]);
}

static void cps1_rebuild_video_state(void)
{
    uint32_t pal_base = cps1_resolve_base(CPSA_REG(REG_PALETTE));
    for (unsigned b = 0; b < CPS1_PALETTE_BANKS; b++)
        for (unsigned c = 0; c < CPS1_PALETTE_COLORS; c++)
            s_pal.colors[b][c] =
                cps1_palette_build(cps1_gfx_word(pal_base + (b * CPS1_PALETTE_COLORS + c) * 2u));

    for (unsigned L = 0; L < CPS1_BG_LAYER_COUNT; L++) {
        uint32_t base = cps1_resolve_base(CPSA_REG(REG_SCROLL1 + L));
        for (unsigned sc = 0; sc < CPS1_BG_MAP_W * CPS1_BG_MAP_H; sc++) {
            unsigned col, row;
            cps1_bg_swizzle_offset_to_col_row(L, sc, &col, &row);
            cps1_bg_cell_t *cell = &s_bg.layers[L].cells[row * CPS1_BG_MAP_W + col];
            cell->code = cps1_gfx_word(base + sc * 4u);
            cell->attr = cps1_gfx_word(base + sc * 4u + 2u);
        }
    }

    s_bg.layers[CPS1_BG_SCROLL1].scroll_x = (int16_t)CPSA_REG(6);
    s_bg.layers[CPS1_BG_SCROLL1].scroll_y = (int16_t)CPSA_REG(7);
    s_bg.layers[CPS1_BG_SCROLL2].scroll_x = (int16_t)CPSA_REG(8);
    s_bg.layers[CPS1_BG_SCROLL2].scroll_y = (int16_t)CPSA_REG(9);
    s_bg.layers[CPS1_BG_SCROLL3].scroll_x = (int16_t)CPSA_REG(10);
    s_bg.layers[CPS1_BG_SCROLL3].scroll_y = (int16_t)CPSA_REG(11);

    uint32_t obj_base = cps1_resolve_base(CPSA_REG(REG_OBJ));
    s_oam.count = 0;
    for (unsigned i = 0; i < CPS1_OAM_MAX_SPRITES; i++) {
        uint16_t x = cps1_gfx_word(obj_base + i * 8u);
        uint16_t y = cps1_gfx_word(obj_base + i * 8u + 2u);
        if (x == 0xFFFF && y == 0xFFFF) break;   /* end marker */
        cps1_oam_entry_t *s = &s_oam.sprites[s_oam.count++];
        s->x = (int16_t)x; s->y = (int16_t)y;
        s->tile_index = cps1_gfx_word(obj_base + i * 8u + 4u);
        s->attr = cps1_gfx_word(obj_base + i * 8u + 6u);
        s->enabled = 1;
    }
}

static void cps1_render_into(uint16_t *fb, uint8_t *meta)
{
    /* Back to front: SCROLL3 (32x32) is the far background, SCROLL1 (8x8) the
     * text/foreground layer. Drawing 1,2,3 paints the background over
     * everything -- that bug turned the title screen into three flat bands. */
    static const unsigned draw_order[CPS1_BG_LAYER_COUNT] = {
        CPS1_BG_SCROLL3, CPS1_BG_SCROLL2, CPS1_BG_SCROLL1
    };
    for (unsigned i = 0; i < CPS1_BG_LAYER_COUNT; i++) {
        unsigned L = draw_order[i];
        cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, fb, meta);
    }
    cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, fb);
}

/* --------------------------------------------------------------- init --- */

/* Sound CPU stand-ins. The Z80 stamps 0x77 at 0xF19FFE to say it is alive
 * (the boot code spins on it), and raises bit 7 of 0xF1801F to say it is ready
 * for a command batch -- with that clear, the vblank handler's `bpl` at 0x5A28
 * skips the entire sound path and the game never advances. Both are HLE
 * stubs; there is no QSound emulation here yet. */
static void cps1_sound_stub_init(void)
{
    memset(s_qram, 0, sizeof(s_qram));
    s_qram[0x1FFF] = 0x77;   /* 0xF19FFE low byte, base 0xF18000 */
    s_qram[0x001F] = 0x80;   /* 0xF1801F bit 7 */
}

static void cps1_machine_reset(const uint8_t *prg, uint32_t prg_size)
{
    memset(s_gfxram, 0, sizeof(s_gfxram));
    memset(s_wram, 0, sizeof(s_wram));
    memset(s_regs, 0, sizeof(s_regs));
    cps1_sound_stub_init();
    cps1_eeprom_reset(&s_eeprom);
    cps1_tile_cache_reset(&s_cache);
    s_cache.real_gfx = 1;
    cps1_bg_reset(&s_bg);

    static const cps1_m68k_io_t io = { cps1_bus_read, cps1_bus_write };
    /* NULL work RAM routes it through the io callbacks rather than a base
     * pointer. Slower per access, but base-mapped WRAM demonstrably stalls
     * this game (the palette never fills) for a reason not yet isolated --
     * see the commit that found it. Do not "optimise" this back without
     * re-testing that the title screen still renders. */
    cps1_m68k_init(prg, prg_size, NULL, &io);
    cps1_m68k_reset();
}

static void cps1_run_one_frame(void)
{
    /* Vblank is a level-2 autovector on CPS-1, and it must be HELD, not
     * pulsed: the game's main loop runs its scheduler under `move.w #$2600,sr`
     * (mask 6 blocks level 2), so a short pulse landing in that window is lost
     * outright and the frame never advances. */
    cps1_m68k_set_irq(2);
    cps1_m68k_run(CPS1_FRAME_MASTER / 2u);
    cps1_m68k_set_irq(0);
    cps1_m68k_run(CPS1_FRAME_MASTER - CPS1_FRAME_MASTER / 2u);
}

void app_main_cps1(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;

    odroid_gamepad_state_t joystick;

    odroid_system_init(APPID_CPS1, CPS1_SAMPLE_RATE);
    odroid_system_emu_init(NULL, NULL, NULL, NULL, NULL, NULL);

    /* TODO(cps1): enumerate the game folder, cache each chip in external
     * flash via odroid_overlay_cache_file_in_flash(), and identify the slots
     * by CRC32 -- never by filename, because wof's tk2_gfx2.rom occupies the
     * THIRD gfx slot while tk2_gfx3.rom occupies the second, and getting that
     * backwards loads cleanly and corrupts only the picture. Until that lands
     * s_rom/s_gfx_chips stay empty and this returns immediately rather than
     * rendering garbage. */
    if (s_rom.prg.data == NULL)
        return;

    cps1_machine_reset(s_rom.prg.data, s_rom.prg.size);

    while (true) {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, NULL, NULL);

        cps1_run_one_frame();

        if (drawFrame) {
            cps1_rebuild_video_state();
            uint16_t *fb = (uint16_t *)lcd_get_active_buffer();
            cps1_render_into(fb, NULL);
            common_ingame_overlay();
            lcd_swap();
        }

        common_emu_sound_loop(NULL);
    }
}
