#include <odroid_system.h>

#include <string.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "gw_flash_alloc.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "bilinear.h"
#include "error_screens.h"

/* gpsp. The core's own headers pull in libretro types and register-name macros
 * that collide with CMSIS, so we declare the handful of entry points we use. */
#include "gba_savestate_abi.h"
#include "gba_idle_loop.h"

extern uint32_t  idle_loop_target_pc; /* gpSP's; gba_frontend.c owns the storage */
extern uint16_t *gba_screen_pixels; /* the core renders straight into this, RGB565 */
extern uint32_t  execute_cycles;    /* cycles the core wants to run before the next event */
extern uint32_t  skip_next_frame;   /* set and the PPU evaluates but does not draw */
extern uint8_t   bios_rom[16 * 1024];
extern uint8_t   gamepak_backup[128 * 1024];
extern const uint8_t open_gba_bios_rom[];

void     init_main(void);
void     init_memory(void);
void     init_sound(void);
void     init_gamepak_buffer(void);
void     reset_gba(void);
void     execute_arm(uint32_t cycles);
void     gba_set_xip_rom(uint8_t *base, uint32_t size);
void     gba_set_keys(uint32_t keys);
uint32_t load_gamepak(const void *info, const char *name, int rtc, int rumble, int serial);
uint32_t sound_read_samples(int16_t *out, uint32_t frames);
#if CHEAT_CODES == 1
/* gpsp's cheat engine: GameShark / CodeBreaker / Action Replay, 20 slots. */
int  cheat_parse(unsigned index, const char *code);
void cheat_clear(void);
#define GBA_MAX_CHEAT_SLOTS 20
#endif

#define GBA_WIDTH   240
#define GBA_HEIGHT  160
#define GBA_FPS     60
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

/* The mixer runs at the rate the SAI is already set to, so nothing has to be
 * resampled on a budget that has no room for it (gpsp's own default is 65536Hz).
 * The device has one speaker, so the core's stereo pair is folded to mono on the
 * way out — half the samples to touch, and nothing is lost that could be heard. */
#define GBA_SAMPLE_RATE          48000
#define GBA_AUDIO_FRAMES         (GBA_SAMPLE_RATE / GBA_FPS)   /* mono samples per frame */
static int16_t gba_audio_stereo[GBA_AUDIO_FRAMES * 2];

/* The GBA framebuffer. The LCD's own buffers live outside RAM_EMU, but the core
 * renders a 240x160 image that then has to be scaled, so it needs a source.
 *
 * It comes from AHB SRAM, not from the overlay: 75 KB of a 724 KB pool that this
 * core has already very nearly spent, against 120 KB of AHB that nothing else is
 * using while a game runs. The scaler reads it once per frame and the DMA never
 * touches it, so the slower bus costs nothing that shows. */
#define GBA_FRAMEBUFFER_BYTES  (GBA_WIDTH * GBA_HEIGHT * sizeof(uint16_t))
static uint16_t *gba_framebuffer;

static odroid_video_frame_t video_frame = {GBA_WIDTH, GBA_HEIGHT, GBA_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static void blit_emulator(void);

/* ------------------------------------------------------------------ fatal --- */
/* Say which failure it was, and stay on screen while it is read.
 *
 * Not an alert-and-return-to-the-launcher: the two ways loading fails here (no
 * room left in the flash cache for the cart, a cart the core will not take) are
 * different problems with different fixes, and a single "load failed" toast that
 * vanishes tells the player neither. Not a sleep either — deep sleep comes back
 * through gw_sleep.c, whose sdcard_init() then fails, and the last thing left on
 * screen is "No SD CARD found": a message about a card that was never the problem
 * (this cost half a day once, on Super Metroid). */
static void __attribute__((noreturn)) gba_fatal(const char *line_1, const char *line_2)
{
    printf("gba: FATAL %s / %s\n", line_1, line_2 ? line_2 : "");
    lcd_backlight_set(180);
    draw_error_screen("GAME BOY ADVANCE", line_1, line_2);

    while (true) {
        wdog_refresh();
        lcd_sync();
        lcd_swap();
        HAL_Delay(20);
    }
}

/* -------------------------------------------------------------------- XIP ---
 * gpSP is 853 KB of core against a 724 KB pool. The scanline renderer (video.o)
 * and the 16 KB BIOS image are linked at a sentinel address instead, shipped as
 * one file — /roms/homebrew/gba.xip — cached into QSPI flash, and executed and
 * read straight out of it. Same trick as Super Metroid's sm.xip; the linker
 * script says which object goes where and why.
 *
 * Code and BIOS share the region, and therefore one cache entry, on purpose: the
 * renderer's own rodata sits between them, and a pointer from one cache entry
 * into another goes stale the moment the circular cache evicts one and not the
 * other. As a single blob every such pointer is a sentinel into the blob itself,
 * so one relocation against one base fixes all of them at once.
 *
 * The relocation happens on the way IN — the cache hands each buffer to
 * gba_relocate_xip() before programming it — rather than by rewriting flash that
 * has already been written. A rewrite would have to erase first, and an erase
 * interrupted by a flat battery leaves a blank hole indistinguishable from a
 * finished job. On a cache hit nothing is written and nothing needs to be: the
 * copy in flash was relocated to that same address when it was first stored.
 */
#define GBA_CODE_BASE  0xDEC00000u
#define GBA_XIP_PATH   "/roms/homebrew/gba.xip"

static uint8_t *g_xip_addr;
static uint32_t g_xip_size;
static int32_t  g_xip_offset;

/* Rewrite every sentinel-range word in [start, end) to where the blob really
 * landed. Thumb bit included, hence the & ~1. */
static int patch_gba_sentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size)
{
    int patched = 0;
    for (uint32_t *p = start; p < end; p++) {
        uint32_t v = *p;
        if ((v & ~1u) >= GBA_CODE_BASE && (v & ~1u) < GBA_CODE_BASE + size) {
            *p = (uint32_t)(v + offset);
            patched++;
        }
    }
    return patched;
}

/* Relocation hook: runs on each buffer of gba.xip on its way into the flash. */
static void gba_relocate_xip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                             uint8_t *file_address, uint32_t file_size)
{
    (void)offset_in_file;
    int32_t offset = (int32_t)((uint32_t)file_address - GBA_CODE_BASE);
    patch_gba_sentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)), offset, file_size);
}

/* Where a thing linked into the blob actually ended up. main_gba.o is the one
 * object the sentinel pass below does not walk (it holds GBA_CODE_BASE itself, and
 * a scan that could not tell the constant from a reference would rewrite the very
 * constant it is built on), so the single blob pointer this file holds — the BIOS
 * image — is relocated by hand, here. */
static const void *gba_xip_ptr(const void *sentinel)
{
    return (const void *)((uint32_t)sentinel + g_xip_offset);
}

static bool gba_cache_xip_to_flash(void)
{
    g_xip_size = 0;
    g_xip_addr = odroid_overlay_cache_file_in_flash_relocate(GBA_XIP_PATH, &g_xip_size, false,
                                                             &gba_relocate_xip);
    if (g_xip_addr == NULL || g_xip_size == 0) {
        printf("gba: %s missing\n", GBA_XIP_PATH);
        return false;
    }
    g_xip_offset = (int32_t)((uint32_t)g_xip_addr - GBA_CODE_BASE);
    printf("gba: xip blob at %p, %lu bytes, offset 0x%08lX\n",
           g_xip_addr, (unsigned long)g_xip_size, (unsigned long)g_xip_offset);

    /* Everything in the overlay that points into the blob — the RAM->XIP call
     * veneers into the renderer, and every reference to its rodata — still holds a
     * sentinel. Fix them before a single line of core code runs.
     *
     * The ITCM image (cpu.o, the interpreter) is deliberately not scanned, and
     * does not need to be: it references nothing in the blob. The linker script
     * keeps its rodata in RAM to guarantee that, and nm confirms it calls no
     * function of the renderer's. */
    int n = patch_gba_sentinels((uint32_t *)_GBA_MAIN_CODE_END,
                                (uint32_t *)_OVERLAY_GBA_BSS_START,
                                g_xip_offset, g_xip_size);
    printf("gba: patched %d sentinel refs in the overlay\n", n);
    return true;
}

/* ------------------------------------------------------------------- SRAM --- */
/* The cart's own save — the one the game writes when you save in-game. This is
 * what a Pokemon player actually cares about; a savestate is a convenience on
 * top of it. */
static void gba_SramSave(void)
{
    char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(gamepak_backup, 1, sizeof(gamepak_backup), f);
        fclose(f);
    }
    free(path);
}

static void gba_SramLoad(void)
{
    char *path = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fread(gamepak_backup, 1, sizeof(gamepak_backup), f);
        fclose(f);
    }
    free(path);
}

/* -------------------------------------------------------------- savestate --- */
/* gpsp builds its savestate as one contiguous bson document, and that document is
 * 416KB because six buffers — iwram, ewram, vram, oam, palette, ioregs — are
 * ~390KB of it. There is no 416KB block free here; the largest is the 300KB LCD
 * pool. So the six go straight to the file and only the rest, 4,333 bytes of it,
 * is ever held in RAM (gba_bulk_regions() / gba_save_state_slim()).
 *
 * Order matters on the way back in: the bulk buffers have to be in place before
 * the slim document is applied, because applying it rebuilds the palette cache
 * from palette_ram — and a load restores state, not the caches derived from it. */
#define GBA_STATE_MAGIC   0x41425347u  /* 'GBAS' */
#define GBA_STATE_VER     1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slim_len;   /* bytes of the bson document that follow */
    uint32_t bulk_len;   /* bytes of raw buffers after that */
} gba_state_header_t;

static bool gba_SaveState(const char *savePathName)
{
    unsigned nreg = 0;
    const gba_bulk_region_t *rg = gba_bulk_regions(&nreg);
    uint8_t *slim = (uint8_t *)lcd_get_active_buffer();   /* borrow the off-screen buffer */

    lcd_wait_for_vblank();
    gba_save_state_slim(slim);

    uint32_t slim_len = *(uint32_t *)slim;
    uint32_t bulk_len = 0;
    for (unsigned i = 0; i < nreg; i++)
        bulk_len += rg[i].len;

    FILE *f = fopen(savePathName, "wb");
    if (f == NULL)
        return false;

    gba_state_header_t h = {GBA_STATE_MAGIC, GBA_STATE_VER, slim_len, bulk_len};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
              fwrite(slim, 1, slim_len, f) == slim_len;
    for (unsigned i = 0; ok && i < nreg; i++)
        ok = fwrite(rg[i].ptr, 1, rg[i].len, f) == rg[i].len;

    fclose(f);
    lcd_clear_active_buffer();
    return ok;
}

static bool gba_LoadState(const char *savePathName)
{
    unsigned nreg = 0;
    const gba_bulk_region_t *rg = gba_bulk_regions(&nreg);

    /* The main loop is blocked for the whole read below, so the audio DMA would
     * otherwise loop its last buffer — a latched tone — until playback resumes. */
    audio_clear_buffers();

    FILE *f = fopen(savePathName, "rb");
    if (f == NULL)
        return false;

    gba_state_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != GBA_STATE_MAGIC ||
        h.version != GBA_STATE_VER || h.slim_len == 0 ||
        h.slim_len > GBA_STATE_SLIM_SIZE) {
        fclose(f);
        return false;
    }

    uint32_t bulk_len = 0;
    for (unsigned i = 0; i < nreg; i++)
        bulk_len += rg[i].len;
    if (h.bulk_len != bulk_len) {   /* a state this build did not write */
        fclose(f);
        return false;
    }

    uint8_t *slim = (uint8_t *)lcd_get_active_buffer();
    bool ok = fread(slim, 1, h.slim_len, f) == h.slim_len;
    for (unsigned i = 0; ok && i < nreg; i++)
        ok = fread(rg[i].ptr, 1, rg[i].len, f) == rg[i].len;
    fclose(f);

    if (!ok)
        return false;

    /* Bulk first, then the document: applying it rebuilds the palette cache out
     * of the palette_ram we just restored. */
    if (!gba_load_state_slim(slim))
        return false;

    lcd_clear_active_buffer();
    return true;
}

static void *gba_Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

/* ------------------------------------------------------------------ audio --- */
static void gba_pcm_submit(void)
{
    uint32_t got = sound_read_samples(gba_audio_stereo, GBA_AUDIO_FRAMES);

    if (common_emu_sound_loop_is_muted())
        return;

    int32_t   factor = common_emu_sound_get_volume();
    int16_t  *out    = audio_get_active_buffer();
    uint16_t  len    = audio_get_buffer_length();

    if (len > GBA_AUDIO_FRAMES)
        len = GBA_AUDIO_FRAMES;

    for (uint16_t i = 0; i < len; i++) {
        /* One speaker: fold the pair rather than throw a channel away. Anything
         * panned hard to the side would otherwise vanish. */
        int32_t mono = (i < got)
            ? ((int32_t)gba_audio_stereo[i * 2] + gba_audio_stereo[i * 2 + 1]) / 2
            : 0;
        out[i] = (int16_t)((mono * factor) >> 8);
    }
}

/* ------------------------------------------------------------------ video --- */
__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (LCD_WIDTH - dest_width) / 2;
    int wpad = (LCD_HEIGHT - dest_height) / 2;

    uint16_t *screen_buf = (uint16_t *)video_frame.buffer;
    uint16_t *dest = lcd_get_active_buffer();

    /* Write every pixel of the buffer, borders included. A separate clear is a
     * ~150KB memset that can overtake the beam and desync the vblank swap. */
    for (int i = 0; i < wpad; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));
    for (int i = wpad + h2; i < LCD_HEIGHT; i++)
        memset(dest + i * LCD_WIDTH, 0, LCD_WIDTH * sizeof(uint16_t));

    for (int i = 0; i < h2; i++) {
        uint16_t *row = dest + (i + wpad) * LCD_WIDTH;
        int y2 = ((i * y_ratio) >> 16);
        const uint16_t *src_row = screen_buf + (y2 * w1);
        for (int j = 0; j < hpad; j++)
            row[j] = 0;
        for (int j = 0; j < w2; j++)
            row[j + hpad] = src_row[(j * x_ratio) >> 16];
        for (int j = hpad + w2; j < LCD_WIDTH; j++)
            row[j] = 0;
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int hpad = (LCD_WIDTH - dest_width) / 2;
    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img = {dest_width, LCD_HEIGHT, 2, ((uint8_t *)dest) + hpad * 2};
    image_t src_img = {video_frame.width, video_frame.height, 2, video_frame.buffer};

    if (hpad > 0)
        memset(dest, 0x00, hpad * 2);

    imlib_draw_image(&dst_img, &src_img, 0, 0, LCD_WIDTH,
                     ((float)dest_width) / ((float)video_frame.width),
                     ((float)LCD_HEIGHT) / ((float)video_frame.height),
                     NULL, -1, 255, NULL, NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

static void blit_emulator(void)
{
    lcd_sleep_while_swap_pending();

    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    static odroid_display_scaling_t last_scaling = -1;
    if (scaling != last_scaling) {
        lcd_clear_buffers();
        last_scaling = scaling;
    }

    /* 240x160 is 3:2. Filling the 240px height would need 360px of width, which
     * the 320px panel does not have — so FIT fills the width instead and letter-
     * boxes: 320x213. */
    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        screen_blit_nn(GBA_WIDTH, GBA_HEIGHT);   /* native, centred */
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            lcd_clear_active_buffer();           /* bilinear does not fill the borders */
            screen_blit_bilinear(LCD_WIDTH);
        } else {
            screen_blit_nn(LCD_WIDTH, 213);
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT)
            screen_blit_bilinear(LCD_WIDTH);
        else
            screen_blit_nn(LCD_WIDTH, LCD_HEIGHT);
        break;
    default:
        screen_blit_nn(LCD_WIDTH, 213);
        break;
    }
}

static void blit(void)
{
    blit_emulator();
    common_ingame_overlay();
}

/* ------------------------------------------------------------------ input --- */
/* KEYINPUT bit order, and what gpsp's gba_set_keys() expects: a set bit is held. */
#define GBA_KEY_A      0x0001
#define GBA_KEY_B      0x0002
#define GBA_KEY_SELECT 0x0004
#define GBA_KEY_START  0x0008
#define GBA_KEY_RIGHT  0x0010
#define GBA_KEY_LEFT   0x0020
#define GBA_KEY_UP     0x0040
#define GBA_KEY_DOWN   0x0080
#define GBA_KEY_R      0x0100
#define GBA_KEY_L      0x0200

static void gba_input_read(odroid_gamepad_state_t *joystick)
{
    uint32_t keys = 0;
    if (joystick->values[ODROID_INPUT_UP])     keys |= GBA_KEY_UP;
    if (joystick->values[ODROID_INPUT_DOWN])   keys |= GBA_KEY_DOWN;
    if (joystick->values[ODROID_INPUT_LEFT])   keys |= GBA_KEY_LEFT;
    if (joystick->values[ODROID_INPUT_RIGHT])  keys |= GBA_KEY_RIGHT;
    if (joystick->values[ODROID_INPUT_A])      keys |= GBA_KEY_A;
    if (joystick->values[ODROID_INPUT_B])      keys |= GBA_KEY_B;
    if (joystick->values[ODROID_INPUT_START])  keys |= GBA_KEY_START;
    if (joystick->values[ODROID_INPUT_SELECT]) keys |= GBA_KEY_SELECT;
    /* The unit has no shoulder buttons. X/Y stand in for L/R — every GBA game
     * that uses them uses them, and there is nowhere else to put them. */
    if (joystick->values[ODROID_INPUT_X])      keys |= GBA_KEY_L;
    if (joystick->values[ODROID_INPUT_Y])      keys |= GBA_KEY_R;

    gba_set_keys(keys);
}

/* ------------------------------------------------------------------- main --- */
void app_main_gba(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    odroid_gamepad_state_t joystick;
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_LAST
    };

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }
    common_emu_state.frame_time_10us = (uint16_t)(100000 / GBA_FPS + 0.5f);
    lcd_set_refresh_rate(GBA_FPS);

    /* The interpreter is the whole CPU here, and it is not cheap. Same scoped,
     * non-persisted mild boost WonderSwan and VB take: level 1 (312MHz), not the
     * maximum. A user who has chosen a higher level keeps it — common_emu_auto_oc()
     * is a floor, not a setting. Reset on exit. */
    common_emu_auto_oc(1);

    /* The BIOS image, the cheat table and the sound ring live in AHB SRAM (see the
     * linker script), which puts them outside .overlay_gba_bss — so the memset in
     * run_internal_emu() that zeroes this core's BSS does not reach them. Nothing
     * else will: AHB holds whatever the last core left there. */
    memset(__gba_ahb_start__, 0, (size_t)(__gba_ahb_end__ - __gba_ahb_start__));

    gba_framebuffer = ahb_malloc(GBA_FRAMEBUFFER_BYTES);
    if (gba_framebuffer == NULL)
        gba_fatal("Out of AHB SRAM", "The 75KB framebuffer could not be allocated");
    memset(gba_framebuffer, 0, GBA_FRAMEBUFFER_BYTES);

    video_frame.buffer = gba_framebuffer;
    gba_screen_pixels = gba_framebuffer;

    odroid_system_init(APPID_GBA, GBA_SAMPLE_RATE);
    odroid_system_emu_init(&gba_LoadState, &gba_SaveState, &gba_Screenshot,
                           NULL, NULL, &gba_SramSave);

    /* Native 240x160 is a small island on a 320x240 panel; FIT is the sane
     * first-run default. Any choice the user makes afterwards is theirs. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    audio_start_playing(GBA_AUDIO_FRAMES);

    /* Before any core code runs: init_main() and everything after it call into the
     * renderer, and those calls are still pointing at the sentinel address until
     * the blob has been cached and the overlay patched. */
    if (!gba_cache_xip_to_flash())
        gba_fatal("Missing " GBA_XIP_PATH, "Re-run the retro-go_update.bin update");

    init_main();
    init_memory();
    init_sound();

    memcpy(bios_rom, gba_xip_ptr(open_gba_bios_rom), sizeof(bios_rom));
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));

    /* The ROM is up to 32MB and stays in external flash, memory-mapped. Nothing
     * is copied into RAM and nothing is paged: the core reads the cart where it
     * lies. Page 0 is the exception — an RTC cart has its GPIO registers *written*
     * into "ROM" at 0x080000C4, and flash does not take writes, so the core keeps
     * a RAM shadow of it. Ruby, Sapphire and Emerald all keep time that way. */
    uint32_t rom_size = 0;
    uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &rom_size, false);
    if (rom == NULL || rom_size == 0)
        gba_fatal("Could not cache the ROM in flash", "The cart may be larger than the free flash");
    gba_set_xip_rom(rom, rom_size);
    init_gamepak_buffer();

    if (load_gamepak(NULL, ACTIVE_FILE->path, 0, 0, 0) != 0)
        gba_fatal("Not a Game Boy Advance ROM", "The header did not check out");

    /* After load_gamepak, on purpose: it is what sets idle_loop_target_pc from
     * gpSP's own gba_over.h, and ours has to win. A game with no busy-wait PC
     * spins through the whole 280,896-cycle frame instead of doing ~75,000 cycles
     * of work and stopping — so this is not a tuning knob, it is the difference
     * between full speed and no chance of it. See gba_idle_loop.c. */
    uint32_t idle_pc = gba_idle_loop_lookup((const char *)&rom[0xAC]);
    if (idle_pc != 0) {
        idle_loop_target_pc = idle_pc;
        printf("gba: idle loop at 0x%08lX\n", (unsigned long)idle_pc);
    }

    gba_SramLoad();
    reset_gba();

#if CHEAT_CODES == 1
    /* After reset: parsing a code installs the hook the engine watches for, and a
     * reset would throw it away again. Only the codes the user actually ticked on
     * for this ROM go in. */
    cheat_clear();
    unsigned slot = 0;
    for (int i = 0; i < ACTIVE_FILE->cheat_count && slot < GBA_MAX_CHEAT_SLOTS; i++) {
        if (odroid_settings_ActiveGameGenieCodes_is_enabled(ACTIVE_FILE->path, i))
            cheat_parse(slot++, ACTIVE_FILE->cheat_codes[i]);
    }
#endif

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    while (true) {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();
        skip_next_frame = drawFrame ? 0 : 1;

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        gba_input_read(&joystick);

        /* execute_arm() returns when the frame driver says the frame is done. */
        execute_arm(execute_cycles);

        if (drawFrame) {
            blit();
            lcd_swap();
        }

        gba_pcm_submit();

        common_emu_sound_sync(false);
    }
}
