/*
 * Renders a REAL Tenchi wo Kurau II frame: boots the game with Musashi, then
 * points this port's actual renderer (cps1_bg.c / cps1_ppu.c) at the gfxram
 * the game itself filled and the CPS-A base registers the game itself
 * programmed. Nothing synthetic is involved on the data side.
 *
 * Why it does NOT go through cps1_core.c: that file still carries the
 * synthetic scene, the toy-CPU frame loop and eleven selftests built on
 * them. Rewiring it is Phase 14 and deserves its own change; doing it here
 * would risk those selftests for no extra information. This harness
 * therefore owns its own bus (same shape as real_rom_probe.c) and converts
 * the resulting gfxram into the renderer's own structures using the SAME
 * layout rules cps1_core.c's bus uses -- cps1_bg_swizzle_offset_to_col_row
 * for tilemap cells, cps1_palette_build for palette words -- so what is
 * measured is the shipping renderer, not a reimplementation of it.
 *
 * What it answers: the graphics half of the 60fps question. CPU cost is
 * already bounded at 18.3% of the frame budget (tools/m7_qemu_rig/
 * rig_cps1_m68k.c); this counts what a real frame actually asks the
 * renderer to do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cps1_m68k.h"
#include "cps1_rom.h"
#include "cps1_ppu.h"
#include "cps1_bg.h"

#define GFXRAM_BYTES 0x30000u
#define GFXRAM_BASE  0x900000u

static uint8_t  s_wram[0x10000];
static uint8_t  s_gfxram[GFXRAM_BYTES];
static uint8_t  s_qram[0x10000];
static uint16_t s_regs[0xC0];      /* 0x800000-0x80017F, word indexed */

/* CPS-A register indices (cps1_core.c's own numbering). */
enum { REG_OBJ = 0, REG_SCROLL1, REG_SCROLL2, REG_SCROLL3, REG_OTHER, REG_PALETTE };
#define CPSA_REG(i) s_regs[(0x100u + (i) * 2u) >> 1]

/* Write-hook target: the main loop at 0x730 dispatches on this byte. */
#define WATCH_ADDR 0xFF5600u
static unsigned s_watch_writes;
static uint32_t s_qread[0x10000/2];   /* QSound shared-RAM read histogram */
static uint16_t s_eeprom_din = 0xFFFF;
/* per-region gfxram write counters, sampled over the LAST boot frames only */
static unsigned s_wr_obj, s_wr_s1, s_wr_s2, s_wr_s3, s_wr_pal, s_wr_other, s_count_on;
static struct { uint32_t pc; uint16_t val; } s_watch[24];
static unsigned s_watch_n;

static uint16_t bus_read16(uint32_t a)
{
    if (a >= 0xFF0000u) {
        uint32_t o = a - 0xFF0000u;
        return (uint16_t)((s_wram[o] << 8) | s_wram[o + 1]);
    }
    if (a >= GFXRAM_BASE && a < GFXRAM_BASE + GFXRAM_BYTES) {
        uint32_t o = a - GFXRAM_BASE;
        return (uint16_t)((s_gfxram[o] << 8) | s_gfxram[o + 1]);
    }
    if (a < 0x800020u) return 0xFFFFu;              /* inputs are ACTIVE LOW */
    if (a >= 0x800000u && a < 0x800180u) return s_regs[(a - 0x800000u) >> 1];
    /*
     * 0xF1C006 is EEPROMIN (MAME cps1.cpp qsound_main_map): the serial
     * 93C46's data-out line, bit 0. The game polls it ~200k times and
     * stalls -- it is waiting for a bit stream we never clock out. Until a
     * real 93C46 exists, hold DO high: an all-ones read is what an
     * unprogrammed/absent EEPROM looks like, and CPS-1 games take the
     * "settings invalid, use defaults" path from it instead of blocking.
     */
    if ((a & ~1u) == 0xF1C006u)
        return s_eeprom_din;
    if (a >= 0xF10000u && a < 0xF20000u) {
        uint32_t o = a - 0xF10000u;
        s_qread[o >> 1]++;
        return (uint16_t)((s_qram[o] << 8) | s_qram[o + 1]);
    }
    return 0xFFFFu;
}

static void bus_write16(uint32_t a, uint16_t v)
{
    if (a >= 0xFF0000u) {
        uint32_t o = a - 0xFF0000u;
        s_wram[o] = (uint8_t)(v >> 8); s_wram[o + 1] = (uint8_t)v;
        if ((a & ~1u) == (WATCH_ADDR & ~1u)) {
            s_watch_writes++;
            if (s_watch_n < 24) { s_watch[s_watch_n].pc = cps1_m68k_get_pc();
                                  s_watch[s_watch_n].val = v; s_watch_n++; }
        }
        return;
    }
    if (a >= GFXRAM_BASE && a < GFXRAM_BASE + GFXRAM_BYTES) {
        uint32_t o = a - GFXRAM_BASE;
        s_gfxram[o] = (uint8_t)(v >> 8); s_gfxram[o + 1] = (uint8_t)v;
        if (s_count_on) {
            if      (o >= 0x14000u) s_wr_pal++;
            else if (o >= 0x10000u) s_wr_s3++;
            else if (o >= 0x0C000u) s_wr_s2++;
            else if (o >= 0x08000u) s_wr_s1++;
            else if (o >= 0x04000u) s_wr_other++;
            else                    s_wr_obj++;
        }
        return;
    }
    if (a >= 0xF10000u && a < 0xF20000u) {
        uint32_t o = a - 0xF10000u;
        s_qram[o] = (uint8_t)(v >> 8); s_qram[o + 1] = (uint8_t)v;
        return;
    }
    if (a >= 0x800000u && a < 0x800180u) s_regs[(a - 0x800000u) >> 1] = v;
}

/* Same rule as cps1_core.c's cps1_resolve_base(): register value * 256,
 * snapped down to a 0x4000 boundary, wrapped into the 192 KB pool with a
 * MODULO (the size is not a power of two -- masking corrupts it). */
static uint32_t resolve_base(uint16_t reg)
{
    uint32_t b = (uint32_t)reg * 256u;
    b &= ~(0x4000u - 1u);
    return b % GFXRAM_BYTES;
}

static uint16_t gfx_word(uint32_t off)
{
    off %= GFXRAM_BYTES;
    return (uint16_t)((s_gfxram[off] << 8) | s_gfxram[off + 1]);
}

typedef struct { char m[4]; uint16_t ver, flags; char set[16];
                 uint32_t po, ps, go, gs, zo, zs, so, ss, cpsb, crc; } hdr_t;

static cps1_rom_t       s_rom;
static cps1_tile_cache_t s_cache;
static cps1_palette_t   s_pal;
static cps1_bg_state_t  s_bg;
static cps1_oam_t       s_oam;
static uint16_t s_fb[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];
static uint8_t  s_meta[CPS1_FB_WIDTH * CPS1_FB_HEIGHT];

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/cps1_rom/wofj.cps1";
    unsigned boot_frames = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 240;

    FILE *f = fopen(path, "rb");
    if (!f) { printf("[frame] cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc((size_t)n);
    if (!blob || fread(blob, 1, (size_t)n, f) != (size_t)n) { printf("[frame] read fail\n"); return 1; }
    fclose(f);
    hdr_t h; memcpy(&h, blob, sizeof(h));
    if (memcmp(h.m, "CPS1", 4)) { printf("[frame] bad magic\n"); return 1; }

    /* --- boot the real game --- */
    s_qram[0x9FFE + 1] = 0x77;                    /* QSound Z80 "alive" stub */
    const cps1_m68k_io_t io = { bus_read16, bus_write16 };
    cps1_m68k_init(blob + h.po, h.ps, NULL, &io); /* NULL => WRAM via io, hookable */
    cps1_m68k_reset();

    const uint32_t FRAME = 166666u * 7u;
    /*
     * Hold vblank asserted for a slice rather than pulsing it briefly: the
     * main loop runs its flag check under `move.w #$2600,sr` (mask 6, which
     * blocks level 2), so a short pulse that lands inside that window is
     * simply LOST -- the line drops before the CPU is ever willing to look.
     * Real hardware holds the line until the handler acknowledges it.
     */
    unsigned handler_hits = 0;
    for (unsigned i = 0; i < boot_frames; i++) {
        if (i == boot_frames - 10) s_count_on = 1;   /* last 10 frames only */
        cps1_m68k_set_irq(2);
        for (unsigned k = 0; k < 8; k++) {
            cps1_m68k_run(FRAME / 16u);
            uint32_t pc = cps1_m68k_get_pc();
            if (pc >= 0x506u && pc < 0x600u) handler_hits++;
        }
        cps1_m68k_set_irq(0);
        cps1_m68k_run(FRAME / 2u);
    }
    printf("[frame] vblank handler PC samples: %u\n", handler_hits);
    printf("[frame] writes to 0x%06x: %u\n", WATCH_ADDR, s_watch_writes);
    for (unsigned i = 0; i < s_watch_n; i++)
        printf("         PC~0x%06x wrote 0x%04x\n", s_watch[i].pc, s_watch[i].val);
    printf("[frame] 0xFF5600 now = 0x%02x\n", s_wram[0x5600]);
    printf("[frame] gfxram writes in LAST 10 frames: OBJ=%u SCROLL1=%u SCROLL2=%u "
           "SCROLL3=%u OTHER=%u PALETTE=%u\n",
           s_wr_obj, s_wr_s1, s_wr_s2, s_wr_s3, s_wr_other, s_wr_pal);
    {   /* which QSound shared-RAM words does the game hammer? */
        unsigned top[6] = {0};
        for (unsigned i = 0; i < 0x10000/2; i++) {
            for (unsigned k = 0; k < 6; k++)
                if (s_qread[i] > s_qread[top[k]]) {
                    for (unsigned j = 5; j > k; j--) top[j] = top[j-1];
                    top[k] = i; break;
                }
        }
        printf("[frame] QSound shared-RAM most-read words:\n");
        for (unsigned k = 0; k < 6; k++)
            if (s_qread[top[k]])
                printf("         0xF1%04x read %u times (val=0x%04x)\n",
                       top[k]*2, s_qread[top[k]],
                       (s_qram[top[k]*2]<<8)|s_qram[top[k]*2+1]);
    }
    printf("[frame] booted %u frames; PC=0x%06x SR=0x%04x\n",
           boot_frames, cps1_m68k_get_pc(), cps1_m68k_get_sr());
    printf("[frame] CPS-A bases: OBJ=%04x SCROLL1=%04x SCROLL2=%04x SCROLL3=%04x "
           "OTHER=%04x PALETTE=%04x\n",
           CPSA_REG(REG_OBJ), CPSA_REG(REG_SCROLL1), CPSA_REG(REG_SCROLL2),
           CPSA_REG(REG_SCROLL3), CPSA_REG(REG_OTHER), CPSA_REG(REG_PALETTE));

    /* --- real gfxram -> renderer structures --- */
    cps1_rom_region_t prg = { blob + h.po, h.ps };
    cps1_rom_region_t gfx = { blob + h.go, h.gs };
    cps1_rom_region_t none = { 0, 0 };
    if (cps1_rom_attach(&s_rom, prg, gfx, none, none) != 0) {
        printf("[frame] rom attach failed\n"); return 1;
    }
    cps1_tile_cache_reset(&s_cache);
    /* Real GFX ROM: decode through MAME's own bitplane layout, not the flat
     * synthetic reader. See cps1_tile_cache_t::layout. */
    s_cache.layout = &CPS1_GFX_LAYOUT_8X8_LEFT;

    /* palette: raw hardware words -> RGB565 via the shipping converter */
    uint32_t pal_base = resolve_base(CPSA_REG(REG_PALETTE));
    unsigned pal_nonzero = 0;
    for (unsigned b = 0; b < CPS1_PALETTE_BANKS; b++)
        for (unsigned c = 0; c < CPS1_PALETTE_COLORS; c++) {
            uint16_t raw = gfx_word(pal_base + (b * CPS1_PALETTE_COLORS + c) * 2u);
            s_pal.colors[b][c] = cps1_palette_build(raw);
            if (raw) pal_nonzero++;
        }

    /* tilemaps: same swizzle the bus uses, read straight out of gfxram */
    cps1_bg_reset(&s_bg);
    unsigned cells_nonzero[CPS1_BG_LAYER_COUNT] = {0};
    for (unsigned L = 0; L < CPS1_BG_LAYER_COUNT; L++) {
        uint32_t base = resolve_base(CPSA_REG(REG_SCROLL1 + L));
        for (unsigned sc = 0; sc < CPS1_BG_MAP_W * CPS1_BG_MAP_H; sc++) {
            unsigned col, row;
            cps1_bg_swizzle_offset_to_col_row(L, sc, &col, &row);
            cps1_bg_cell_t *cell = &s_bg.layers[L].cells[row * CPS1_BG_MAP_W + col];
            cell->code = gfx_word(base + sc * 4u);
            cell->attr = gfx_word(base + sc * 4u + 2u);
            if (cell->code) cells_nonzero[L]++;
        }
    }
    printf("[frame] palette words non-zero: %u/%u   tilemap cells non-zero: "
           "SCROLL1=%u SCROLL2=%u SCROLL3=%u (of %u each)\n",
           pal_nonzero, CPS1_PALETTE_BANKS * CPS1_PALETTE_COLORS,
           cells_nonzero[0], cells_nonzero[1], cells_nonzero[2],
           CPS1_BG_MAP_W * CPS1_BG_MAP_H);

    /* sprites: OBJ RAM is a flat list of 4-word entries at the OBJ base */
    uint32_t obj_base = resolve_base(CPSA_REG(REG_OBJ));
    s_oam.count = 0;
    for (unsigned i = 0; i < CPS1_OAM_MAX_SPRITES; i++) {
        uint16_t x = gfx_word(obj_base + i * 8u);
        uint16_t y = gfx_word(obj_base + i * 8u + 2u);
        uint16_t code = gfx_word(obj_base + i * 8u + 4u);
        uint16_t attr = gfx_word(obj_base + i * 8u + 6u);
        if (x == 0xFFFF && y == 0xFFFF) break;      /* end marker */
        cps1_oam_entry_t *s = &s_oam.sprites[s_oam.count++];
        s->x = (int16_t)x; s->y = (int16_t)y;
        s->tile_index = code; s->attr = attr; s->enabled = 1;
    }
    printf("[frame] OBJ entries parsed: %u\n", s_oam.count);

    /* --- render with the shipping renderer --- */
    memset(s_fb, 0, sizeof(s_fb));
    memset(s_meta, 0, sizeof(s_meta));
    for (unsigned L = 0; L < CPS1_BG_LAYER_COUNT; L++)
        cps1_bg_render_layer(&s_bg.layers[L], L, &s_rom, &s_cache, &s_pal, s_fb, s_meta);
    cps1_ppu_render(&s_oam, &s_rom, &s_cache, &s_pal, s_fb);

    /* --- is it a picture, or noise/blank? --- */
    unsigned nonzero = 0, distinct = 0;
    static uint8_t seen[65536];
    for (unsigned i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
        if (s_fb[i]) nonzero++;
        if (!seen[s_fb[i]]) { seen[s_fb[i]] = 1; distinct++; }
    }
    printf("[frame] framebuffer: %u/%u non-zero pixels (%.1f%%), %u distinct colours\n",
           nonzero, CPS1_FB_WIDTH * CPS1_FB_HEIGHT,
           100.0 * nonzero / (CPS1_FB_WIDTH * CPS1_FB_HEIGHT), distinct);
    printf("[frame] tile cache: %u hits, %u misses\n", s_cache.hits, s_cache.misses);

    FILE *o = fopen("/tmp/cps1_rom/frame.ppm", "wb");
    if (o) {
        fprintf(o, "P6\n%d %d\n255\n", CPS1_FB_WIDTH, CPS1_FB_HEIGHT);
        for (unsigned i = 0; i < CPS1_FB_WIDTH * CPS1_FB_HEIGHT; i++) {
            uint16_t p = s_fb[i];
            uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 0x1F) << 3),
                               (uint8_t)(((p >> 5) & 0x3F) << 2),
                               (uint8_t)((p & 0x1F) << 3) };
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
        printf("[frame] wrote /tmp/cps1_rom/frame.ppm\n");
    }
    free(blob);
    return 0;
}
