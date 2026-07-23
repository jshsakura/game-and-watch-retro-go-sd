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
#include <sys/stat.h>

#include "appid.h"
#include "odroid_system.h"
#include "common.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include <stdio.h>
#include <stdbool.h>

#include "ff.h"
#include "rom_manager.h"
#include "odroid_overlay.h"
#include "gw_flash_alloc.h"
#include "error_screens.h"

#include "cps1_m68k.h"
#include "cps1_rom.h"
#include "cps1_romset.h"
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
/* Filled by cps1_load_folder_roms(); read in place by cps1_gfx_chip_byte() so
 * the 8 GFX chips cost no RAM. */
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

/* ------------------------------------------------------- XIP relocation --- */
/*
 * Musashi's text and its ~328 KB of constant tables are linked at a SENTINEL
 * address (CPS1_CODE, 0xDED00000) and shipped as /cores/cps1.xip, not loaded
 * into RAM_EMU -- they do not fit alongside CPS-1's own 422 KB of BSS, which is
 * gfxram + work RAM + tilemaps and therefore not negotiable. At startup the
 * blob is cached into external flash, and every pointer that still names the
 * sentinel is patched to the real flash address.
 *
 * This is the .xip_segacd recipe verbatim (Sega CD XIPs the same m68kcpu.c for
 * the same reason), including the detail that main_cps1.o must stay RAM
 * resident: it holds CPS1_CODE_BASE and runs the patch pass, so it has to be
 * executable before the blob exists.
 */
#define CPS1_CODE_BASE 0xDEF00000u
#define CPS1_XIP_PATH  "/cores/cps1.xip"

extern uint8_t *odroid_overlay_cache_file_in_flash_relocate(
    const char *file_path, uint32_t *file_size_p, bool byte_swap,
    void (*relocate_cb)(uint8_t *, uint32_t, uint32_t, uint8_t *, uint32_t));

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;

static void cps1_patch_sentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size)
{
    for (uint32_t *p = start; p < end; p++) {
        uint32_t v = *p;
        /* &~1 so Thumb function pointers (LSB set) match too. */
        if ((v & ~1u) >= CPS1_CODE_BASE && (v & ~1u) < CPS1_CODE_BASE + size)
            *p = (uint32_t)(v + offset);
    }
}

static void cps1_relocate_xip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                              uint8_t *file_address, uint32_t file_size)
{
    (void)offset_in_file;
    int32_t offset = (int32_t)((uint32_t)file_address - CPS1_CODE_BASE);
    cps1_patch_sentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)),
                          offset, file_size);
}

static bool cps1_cache_xip_to_flash(void)
{
    g_xip_size = 0;
    g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(CPS1_XIP_PATH, &g_xip_size,
                                                              false, &cps1_relocate_xip);
    if (g_xip_addr == NULL || g_xip_size == 0) {
        printf("cps1: %s missing\n", CPS1_XIP_PATH);
        return false;
    }
    int32_t off = (int32_t)((uint32_t)g_xip_addr - CPS1_CODE_BASE);
    printf("cps1: xip blob at %p, %lu bytes, offset 0x%08lX\n",
           g_xip_addr, (unsigned long)g_xip_size, (unsigned long)off);
    cps1_patch_sentinels((uint32_t *)__RAM_EMU_START__, (uint32_t *)__RAM_EMU_END__,
                          off, g_xip_size);
    return true;
}

/* -------------------------------------------------- folder ROM loader --- */
/*
 * A CPS-1 "game" on the SD card is a folder holding an extracted MAME romset:
 * a dozen or so chip dumps, no index file, no container. The launcher lists
 * the folder itself as the entry (emulator_is_folder_rom_system), so what
 * arrives here in ACTIVE_FILE->path is a directory.
 *
 * Loading costs no RAM. Each chip is cached into external flash by
 * odroid_overlay_cache_file_in_flash(), which hands back a memory-mapped (XIP)
 * pointer -- the 4 MB of graphics are read where they lie, through
 * cps1_gfx_chip_byte()'s address arithmetic, because an assembled GFX region
 * is 4 MB against a 724 KB RAM_EMU.
 *
 * WHY THE CRC PASS RUNS OVER FLASH AND NOT OVER THE SD CARD: chips have to be
 * identified by content hash (cps1_romset.h), and hashing 5 MB off the card
 * would add seconds to every single launch. Caching first and hashing the XIP
 * copy costs one SD pass on the first launch and none at all after that,
 * because the second launch is a cache hit and the bytes are already mapped.
 *
 * THE PARENT-SET PROBLEM. A MAME clone archive holds only the chips unique to
 * it: wofj's own zip has its two program chips and the four upper graphics
 * chips, and none of the four lower ones, which are byte-identical to the
 * parent's. Requiring every game folder to be self-contained would store those
 * shared chips once per clone -- twice on the card, and twice in the flash
 * cache, which keys on path and so cannot tell that two paths hold identical
 * bytes. So missing chips are fetched from a shared pool:
 *
 *     /roms/cps1/<game>/            the game's own chips, original MAME names
 *     /roms/cps1/<game>/<set>/      OR one subfolder per archive, pooled
 *     /roms/cps1/.shared/<crc>.bin  chips common to several sets, stored once
 *
 * The launcher's folder scan skips names beginning with '.', so .shared never
 * appears as a game.
 *
 * The middle form is what people actually produce: unzip the clone and the
 * parent side by side and the game folder is self-contained, with nothing
 * stored twice under two different names. Chips are keyed by CRC32, so pooling
 * two subfolders is safe -- each set picks its own chips out of the pool. It
 * costs nothing when unused (the subfolder scan only runs if the flat layout
 * did not already produce a set) and one level only, so the scan cannot wander
 * off across the card. Pooling can leave MORE THAN ONE set runnable, and that
 * is a choice the player makes, not one the table order makes for them --
 * cps1_ask_which_set().
 *
 * WHY THE SHARED POOL IS NAMED BY CRC AND NOT BY THE ORIGINAL FILENAME. Two
 * reasons, and both are fatal to the obvious design:
 *
 *   1. MAME filenames are unique only within a game family. Every Street
 *      Fighter II revision ships chips called s92_*.rom whose CONTENTS differ,
 *      so a flat pool of original names collides and silently overwrites.
 *      A CRC32 cannot collide with a different chip -- it IS the identity this
 *      whole loader keys on, so making it the filename simply says so out loud.
 *   2. A pool that has to be SCANNED is a pool every launch pays for. With ten
 *      games' parents in it, booting wofj would cache 40 MB of chips belonging
 *      to other games. Naming by CRC turns the search into a direct open of
 *      exactly the chips this romset is missing -- no scan, nothing cached that
 *      the game does not use.
 *
 * The name is checked, not trusted: a chip fetched as <crc>.bin is hashed like
 * any other and dropped if it does not hash to its own name.
 */
#define CPS1_MAX_FOLDER_CHIPS 24
#define CPS1_SHARED_CHIP_DIR  "/roms/cps1/.shared"

static const uint8_t *s_chip_addr[CPS1_MAX_FOLDER_CHIPS];
static uint32_t       s_chip_crc[CPS1_MAX_FOLDER_CHIPS];
static unsigned       s_chip_count;
/* The two program chips, in 68000 address order. */
static const uint8_t *s_prg_lo;
static const uint8_t *s_prg_hi;

/* Caches one chip and records its hash. `expect_crc` non-zero means the caller
 * already knows what this file must hash to (the shared pool, where the name
 * is the hash) and the chip is rejected rather than trusted if it does not. */
static bool cps1_cache_one_chip_expect(const char *dir_path, const char *name,
                                        uint32_t expect_crc)
{
    if (s_chip_count >= CPS1_MAX_FOLDER_CHIPS)
        return false;

    char path[RG_PATH_MAX + 1];
    int n = snprintf(path, sizeof(path), "%s/%s", dir_path, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        printf("cps1: path too long, skipping %s\n", name);
        return false;
    }

    uint32_t size = 0;
    /* byte_swap = false. MAME applies ROM_REVERSE when it assembles its
     * big-endian program region; a little-endian *(uint16*) read of the raw
     * chip undoes exactly that, so the two cancel and the correct thing to
     * store is the file verbatim. See cps1_m68k_map_prg_chip()'s comment for
     * the reset vector that proves it on the real dump. */
    const uint8_t *addr = odroid_overlay_cache_file_in_flash(path, &size, false);
    if (addr == NULL || size != CPS1_ROMSET_CHIP_SIZE) {
        printf("cps1: %s did not cache (size %lu)\n", name, (unsigned long)size);
        return false;
    }

    uint32_t crc = cps1_crc32(addr, size);
    if (expect_crc != 0 && crc != expect_crc) {
        printf("cps1: %s hashes to %08lX, not its own name\n", path, (unsigned long)crc);
        return false;
    }
    /* A chip present in both the game folder and the shared pool would
     * otherwise occupy two flash-cache entries and two live-file slots for one
     * set of bytes. */
    for (unsigned i = 0; i < s_chip_count; i++)
        if (s_chip_crc[i] == crc)
            return true;

    s_chip_addr[s_chip_count] = addr;
    s_chip_crc[s_chip_count] = crc;
    s_chip_count++;
    return true;
}

static bool cps1_cache_one_chip(const char *dir_path, const char *name)
{
    return cps1_cache_one_chip_expect(dir_path, name, 0);
}

/* Pulls exactly the chips `set` is missing out of the shared pool, by hash.
 * Returns how many were added. */
static unsigned cps1_fetch_missing_from_shared(const cps1_romset_t *set)
{
    unsigned added = 0;
    uint32_t wanted[CPS1_ROMSET_PRG_CHIPS + CPS1_ROMSET_GFX_CHIPS];
    unsigned want_count = 0;

    for (unsigned i = 0; i < CPS1_ROMSET_PRG_CHIPS; i++)
        wanted[want_count++] = set->prg_crc[i];
    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        wanted[want_count++] = set->gfx_crc[i];

    for (unsigned w = 0; w < want_count; w++) {
        wdog_refresh();
        bool have = false;
        for (unsigned i = 0; i < s_chip_count && !have; i++)
            have = (s_chip_crc[i] == wanted[w]);
        if (have)
            continue;

        char name[16];
        snprintf(name, sizeof(name), "%08lx.bin", (unsigned long)wanted[w]);
        if (cps1_cache_one_chip_expect(CPS1_SHARED_CHIP_DIR, name, wanted[w]))
            added++;
    }
    return added;
}

/* Caches every chip-sized file in one directory. */
static void cps1_scan_chip_dir(const char *dir_path)
{
    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, dir_path) != FR_OK)
        return;

    while (s_chip_count < CPS1_MAX_FOLDER_CHIPS) {
        wdog_refresh();
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (fno.fname[0] == '.' || (fno.fattrib & AM_DIR))
            continue;
        /* Every CPS-1 chip in every supported set is 512 KB, so this skips
         * the PAL dumps (279 B) and any stray file without reading it. */
        if (fno.fsize != CPS1_ROMSET_CHIP_SIZE)
            continue;
        cps1_cache_one_chip(dir_path, fno.fname);
    }
    f_closedir(&dir);
}

/*
 * ONE LEVEL OF SUBFOLDERS, POOLED.
 *
 * The layout above -- chips loose in the game folder -- is not the one people
 * actually end up with. A MAME clone needs its parent's chips, and the natural
 * way to keep a game self-contained is one subfolder per archive:
 *
 *     /roms/cps1/Warriors of Fate/wof/     the parent set, complete
 *     /roms/cps1/Warriors of Fate/wofj/    the Japan clone, 6 chips
 *
 * Chips are identified by CRC32, never by filename or position, so pooling two
 * subfolders into one chip list is safe: whichever set is asked for picks its
 * own chips out of the pool and ignores the rest. This is the same thing
 * /roms/cps1/.shared does, except the pool is local, the folder stays
 * self-contained, and nothing has to be stored twice under two names.
 *
 * ONE LEVEL ONLY, and deliberately: a recursive scan would wander into
 * whatever else is on the card, and every extra directory is SD reads and
 * flash cache entries spent before the game starts.
 *
 * MORE THAN ONE SET CAN COME OUT OF THE POOL -- wof and wofj both do, from the
 * two folders above. cps1_load_folder_roms() enumerates them and asks; see
 * there. This function only gathers.
 */
static void cps1_scan_chip_subdirs(const char *dir_path)
{
    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, dir_path) != FR_OK)
        return;

    while (s_chip_count < CPS1_MAX_FOLDER_CHIPS) {
        wdog_refresh();
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0)
            break;
        if (!(fno.fattrib & AM_DIR) || fno.fname[0] == '.')
            continue;

        char sub[RG_PATH_MAX + 1];
        int n = snprintf(sub, sizeof(sub), "%s/%s", dir_path, fno.fname);
        if (n < 0 || (size_t)n >= sizeof(sub)) {
            printf("cps1: subfolder path too long, skipping %s\n", fno.fname);
            continue;
        }
        cps1_scan_chip_dir(sub);
    }
    f_closedir(&dir);
}

/*
 * Every romset the gathered chips can actually run. Usually one. Two when a
 * game folder holds a clone beside its parent, which is exactly the case
 * cps1_romset_match() answers wrongly -- it returns whichever comes first in
 * the generated table, a choice nobody made and the player cannot change.
 */
#define CPS1_MAX_RUNNABLE_SETS 6

static unsigned cps1_collect_runnable_sets(const cps1_romset_t *out[CPS1_MAX_RUNNABLE_SETS])
{
    unsigned n = 0;
    int prg[CPS1_ROMSET_PRG_CHIPS], gfx[CPS1_ROMSET_GFX_CHIPS];

    for (unsigned s = 0; s < cps1_romset_count && n < CPS1_MAX_RUNNABLE_SETS; s++) {
        if (cps1_romset_resolve(&cps1_romsets[s], s_chip_crc, s_chip_count, prg, gfx) == 0)
            out[n++] = &cps1_romsets[s];
    }
    return n;
}

/*
 * Which one does the player want? Only asked when the answer is genuinely
 * ambiguous -- one runnable set launches straight into the game, as it always
 * did. Two or more (World beside Japan, say) is a real choice and guessing it
 * would mean one of the two sets on the card could never be played at all.
 *
 * Returns the chosen set, or NULL if the dialog was dismissed.
 */
static const cps1_romset_t *cps1_ask_which_set(const cps1_romset_t *const *sets, unsigned count)
{
    odroid_dialog_choice_t options[CPS1_MAX_RUNNABLE_SETS + 1];

    for (unsigned i = 0; i < count; i++) {
        options[i].id = (int)i;
        options[i].label = sets[i]->name;
        options[i].value = (char *)"";
        options[i].enabled = 1;
        options[i].update_cb = NULL;
    }
    options[count] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;

    int sel = odroid_overlay_dialog("Which version?", options, 0, NULL, 0);
    if (sel < 0 || (unsigned)sel >= count)
        return NULL;
    return sets[sel];
}

/* The chips are cached and hashed into s_chip_addr/s_chip_crc by now, however
 * they arrived (loose folder, pooled subfolders, or a .cps1 container). From
 * here the two paths are identical: pick the runnable set, ask if ambiguous,
 * fetch anything missing from the shared pool, and bind chips to slots. `label`
 * names the source in logs/errors; `hint` is the third error-screen line. */
static const cps1_romset_t *cps1_resolve_and_attach(const char *label, const char *hint)
{
    int prg_index[CPS1_ROMSET_PRG_CHIPS], gfx_index[CPS1_ROMSET_GFX_CHIPS];
    const cps1_romset_t *set = NULL;
    {
        const cps1_romset_t *runnable[CPS1_MAX_RUNNABLE_SETS];
        unsigned n = cps1_collect_runnable_sets(runnable);
        if (n == 1) {
            set = runnable[0];
        } else if (n > 1) {
            /* Pooled subfolders can complete several sets at once. Do not
             * pick for the player: the unchosen one would be unplayable
             * forever, and "first in the generated table" is not an answer
             * anyone could predict or change. */
            printf("cps1: %u runnable sets in %s, asking\n", n, label);
            set = cps1_ask_which_set(runnable, n);
            if (set == NULL) {
                printf("cps1: version dialog dismissed\n");
                return NULL;
            }
        }
        if (set != NULL &&
            cps1_romset_resolve(set, s_chip_crc, s_chip_count, prg_index, gfx_index) != 0)
            set = NULL;   /* cannot happen: it resolved a moment ago */
    }
    if (set == NULL) {
        /* Incomplete on its own -- the ordinary case for a clone archive. Work
         * out which set it is closest to and fetch precisely that set's
         * missing chips from the shared pool, by hash. Nothing else is read
         * and nothing else is cached, however many other games' parents happen
         * to live there. */
        unsigned missing = 0;
        const cps1_romset_t *near = cps1_romset_closest(s_chip_crc, s_chip_count, &missing);
        if (near != NULL && missing > 0) {
            unsigned added = cps1_fetch_missing_from_shared(near);
            if (added > 0) {
                printf("cps1: %s needed %u chips, %u came from %s\n",
                       near->name, missing, added, CPS1_SHARED_CHIP_DIR);
                set = cps1_romset_match(s_chip_crc, s_chip_count, prg_index, gfx_index);
            }
        }
    }
    if (set == NULL) {
        /*
         * Say WHICH set the folder nearly is and HOW MANY chips are absent.
         * The overwhelmingly common cause is a clone archive extracted on its
         * own, and "wofj: 4 of 10 chips missing" points at the parent set,
         * where a bare failure looks exactly like an unsupported game. This
         * goes on screen, not just down the log -- a folder problem the player
         * cannot see is a folder problem they cannot fix.
         */
        unsigned missing = 0;
        const cps1_romset_t *near = cps1_romset_closest(s_chip_crc, s_chip_count, &missing);
        char line[64];
        if (near != NULL)
            snprintf(line, sizeof(line), "%s: %u of %u chips missing", near->name, missing,
                     CPS1_ROMSET_PRG_CHIPS + CPS1_ROMSET_GFX_CHIPS);
        else
            snprintf(line, sizeof(line), "%u chips found, no set matched", s_chip_count);
        printf("cps1: incomplete romset in %s -- %s\n", label, line);
        draw_error_screen("Incomplete CPS-1 romset", line, hint);
        return NULL;
    }

    for (unsigned i = 0; i < CPS1_ROMSET_GFX_CHIPS; i++)
        s_gfx_chips.chip[i] = s_chip_addr[gfx_index[i]];
    s_gfx_chips.chip_size = CPS1_ROMSET_CHIP_SIZE;
    s_gfx_chips.chip_count = CPS1_ROMSET_GFX_CHIPS;

    s_prg_lo = s_chip_addr[prg_index[0]];
    s_prg_hi = s_chip_addr[prg_index[1]];

    cps1_rom_region_t prg = { s_prg_lo, CPS1_ROMSET_CHIP_SIZE };
    if (cps1_rom_attach_chips(&s_rom, prg, &s_gfx_chips) != 0) {
        printf("cps1: rom attach failed\n");
        return NULL;
    }
    /* prg.size describes chip 0 only; the whole program is both chips, and
     * cps1_rom_check_reset_vector() only ever looks at the first eight bytes,
     * which live in chip 0. */
    uint32_t ssp = 0, pc = 0;
    if (cps1_rom_check_reset_vector(&s_rom.prg, &ssp, &pc) != 0) {
        printf("cps1: %s reset vector rejected (SSP=%08lX PC=%08lX)\n",
               set->name, (unsigned long)ssp, (unsigned long)pc);
        return NULL;
    }

    printf("cps1: %s loaded, %u chips, SSP=%08lX PC=%08lX\n",
           set->name, s_chip_count, (unsigned long)ssp, (unsigned long)pc);
    return set;
}

static const cps1_romset_t *cps1_load_folder_roms(const char *dir_path)
{
    s_chip_count = 0;

    cps1_scan_chip_dir(dir_path);
    /* Chips loose in the game folder are the simple case and stay the fast
     * path; only reach for the subfolders when that did not produce a set. */
    if (cps1_romset_match(s_chip_crc, s_chip_count, (int[CPS1_ROMSET_PRG_CHIPS]){0},
                           (int[CPS1_ROMSET_GFX_CHIPS]){0}) == NULL)
        cps1_scan_chip_subdirs(dir_path);

    return cps1_resolve_and_attach(dir_path, "Put the parent set in a subfolder beside it");
}

/*
 * A .cps1 container is the raw concatenation of a game's distinct 512 KB chips,
 * uncompressed, no header: the whole game in one file. It is what the library
 * ships (game-and-what pre-builds it at upload) and what the player drops on the
 * card -- one file, like a .nes, instead of a folder of chip dumps.
 *
 * Loading it is the folder scan minus the directory walk: each 512 KB block is
 * cached into flash as its own region (store_file_region_in_flash keys the cache
 * on path+offset, so blocks do not collide), hashed, and recorded exactly as a
 * loose chip would be. Chips bind to romset slots by CRC, never by position, so
 * the order inside the container is irrelevant and no index is needed. One fopen
 * for the whole game -- not one per chip -- so the 10-slot descriptor table is
 * never a constraint the way a 20-file folder made it.
 */
static bool cps1_cache_one_block(const char *path, uint32_t offset)
{
    if (s_chip_count >= CPS1_MAX_FOLDER_CHIPS)
        return false;

    uint32_t size = 0;
    const uint8_t *addr = odroid_overlay_cache_file_region_in_flash(path, offset,
                                                                    CPS1_ROMSET_CHIP_SIZE, &size);
    if (addr == NULL || size != CPS1_ROMSET_CHIP_SIZE) {
        printf("cps1: block @%lu did not cache (size %lu)\n",
               (unsigned long)offset, (unsigned long)size);
        return false;
    }

    uint32_t crc = cps1_crc32(addr, size);
    for (unsigned i = 0; i < s_chip_count; i++)
        if (s_chip_crc[i] == crc)
            return true;   /* a container should hold no duplicates, but be safe */

    s_chip_addr[s_chip_count] = addr;
    s_chip_crc[s_chip_count] = crc;
    s_chip_count++;
    return true;
}

static const cps1_romset_t *cps1_load_container_file(const char *file_path)
{
    s_chip_count = 0;

    struct stat st;
    if (stat(file_path, &st) != 0 || st.st_size < (long)CPS1_ROMSET_CHIP_SIZE ||
        (st.st_size % CPS1_ROMSET_CHIP_SIZE) != 0) {
        printf("cps1: %s is not a valid container (size %ld)\n",
               file_path, (long)st.st_size);
        draw_error_screen("Bad CPS-1 file", "Not a valid .cps1 container",
                          "Re-download this game");
        return NULL;
    }

    unsigned blocks = (unsigned)(st.st_size / CPS1_ROMSET_CHIP_SIZE);
    for (unsigned b = 0; b < blocks && s_chip_count < CPS1_MAX_FOLDER_CHIPS; b++) {
        wdog_refresh();
        cps1_cache_one_block(file_path, b * CPS1_ROMSET_CHIP_SIZE);
    }

    return cps1_resolve_and_attach(file_path, "Re-download this game");
}

/* True when the launched entry is a single .cps1 container file rather than a
 * game folder. The launcher lists both during the transition. */
static bool cps1_path_is_container(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && ((dot[1] | 0x20) == 'c') && ((dot[2] | 0x20) == 'p')
               && ((dot[3] | 0x20) == 's') && (dot[4] == '1') && (dot[5] == '\0');
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

static void cps1_machine_reset(const uint8_t *prg_lo, const uint8_t *prg_hi)
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
    /* prg = NULL: the two program chips are separate cached files at
     * unrelated flash addresses, so each is mapped over its own 64 KB pages
     * instead of through one base pointer over a contiguous 1 MB. */
    cps1_m68k_init(NULL, 0, NULL, &io);
    cps1_m68k_map_prg_chip(0x000000u, prg_lo, CPS1_ROMSET_CHIP_SIZE);
    cps1_m68k_map_prg_chip(0x080000u, prg_hi, CPS1_ROMSET_CHIP_SIZE);
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
    odroid_system_emu_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    /* Musashi lives in XIP flash; nothing below can call it until this runs. */
    if (!cps1_cache_xip_to_flash())
        return;

    /* Anything this core ram_malloc()s must start past its own overlay+bss --
     * the main_gwenesis.c pattern; getting it wrong allocates over the core's
     * own code. */
    ram_start = (uint32_t)&_OVERLAY_CPS1_BSS_END;

    if (ACTIVE_FILE == NULL)
        return;
    /* A .cps1 file is the whole game in one container; a folder is the older
     * loose-chip / subfolder layout. Both still load. */
    const cps1_romset_t *set = cps1_path_is_container(ACTIVE_FILE->path)
                                   ? cps1_load_container_file(ACTIVE_FILE->path)
                                   : cps1_load_folder_roms(ACTIVE_FILE->path);
    if (set == NULL)
        return;

    cps1_machine_reset(s_prg_lo, s_prg_hi);

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

        /* No sound yet -- the QSound side is stubbed (see cps1_sound_stub_init).
         * Still call the sync so the frame pacing the mixer drives stays
         * correct rather than free-running. */
        common_emu_sound_sync(false);
    }
}
