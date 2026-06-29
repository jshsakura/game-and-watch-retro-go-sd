/* Commodore 64 porting layer (floooh/chips c64.h core).
 * Video + joystick + .prg autostart first; SID audio is a no-op callback for v1
 * (added later, like ZX/PCE-CD shipping before their audio). Mirrors the
 * zx/videopac/pce porting pattern. .crt/.d64 are later phases. */
#include <odroid_system.h>
#include <string.h>

#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include "rom_manager.h"
#include "main_c64.h"

#include "chips/chips_common.h"
#include "chips/m6502.h"
#include "chips/m6526.h"
#include "chips/m6522.h"
#include "chips/m6569.h"
#include "chips/m6581.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/c1530.h"
#include "chips/c1541.h"
#include "chips/c64.h"

#define C64_AUDIO_SAMPLE_RATE 22050
#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define C64_FRAME_US 20000          /* one 50 Hz PAL frame in microseconds */

/* The M6569 framebuffer is 504x312 (stride = M6569_FRAMEBUFFER_WIDTH). The 320x200
 * active text area sits at framebuffer (100,60); crop a 320x240 window starting at
 * (100,40) so the 320-wide LCD shows the full picture width + 20px border top/bottom. */
#define C64_CROP_X 100
#define C64_CROP_Y 40

static c64_t     c64;
static uint16_t  c64_pal565[256];

static void audio_cb(const float *s, int n, void *u) { (void)s; (void)n; (void)u; }

/* ---- save/load: dump the live c64_t straight to disk. It is a static at a fixed
 * address in the overlay, so its internal self-pointers (mem page table -> ram/rom)
 * and the audio callback ptr stay valid after reload within the same firmware build.
 * Avoids the chips snapshot API, whose c64_load_snapshot keeps a full static scratch
 * c64_t that would blow the RAM_EMU overlay budget. ---- */
static bool SaveState(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&c64, sizeof(c64), 1, f);
    fclose(f);
    return true;
}

static bool LoadState(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fread(&c64, sizeof(c64), 1, f);
    fclose(f);
    return true;
}

static void c64_build_palette(void)
{
    chips_display_info_t di = c64_display_info(&c64);
    const uint32_t *pal = (const uint32_t *)di.palette.ptr;   /* 0xAABBGGRR */
    int n = (int)(di.palette.size / sizeof(uint32_t));
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        uint32_t c = pal[i];
        c64_pal565[i] = RGB565(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
    }
}

static void c64_blit(void)
{
    const uint8_t *fb = c64.fb;     /* 8bpp colour indices, stride 504 */
    uint16_t *out = (uint16_t *)lcd_get_inactive_buffer();
    for (int y = 0; y < 240; y++) {
        const uint8_t *src = &fb[(C64_CROP_Y + y) * M6569_FRAMEBUFFER_WIDTH + C64_CROP_X];
        uint16_t *dst = &out[y * WIDTH];
        for (int x = 0; x < 320; x++)
            dst[x] = c64_pal565[src[x]];
    }
}

static const uint8_t *cache_rom(const char *path, uint32_t want)
{
    uint32_t sz = 0;
    const uint8_t *p = (const uint8_t *)odroid_overlay_cache_file_in_flash(path, &sz, false);
    if (!p || sz < want) {
        printf("[C64] missing %s (got %u, want %u)\n", path, (unsigned)sz, (unsigned)want);
        return NULL;
    }
    return p;
}

static bool load_bios(c64_desc_t *desc)
{
    /* firmware /bios/<sys>/ convention; each cached in flash, c64_init memcpy's
     * them into core RAM. ROMs are user-supplied (copyright) — never bundled. */
    const uint8_t *chars  = cache_rom("/bios/c64/chargen.bin", 4096);
    const uint8_t *basic  = cache_rom("/bios/c64/basic.bin",   8192);
    const uint8_t *kernal = cache_rom("/bios/c64/kernal.bin",  8192);
    if (!chars || !basic || !kernal) return false;
    desc->roms.chars.ptr  = (void *)chars;  desc->roms.chars.size  = 4096;
    desc->roms.basic.ptr  = (void *)basic;  desc->roms.basic.size  = 8192;
    desc->roms.kernal.ptr = (void *)kernal; desc->roms.kernal.size = 8192;
    return true;
}

static void type_char(uint8_t ascii)
{
    c64_key_down(&c64, ascii);
    c64_exec(&c64, C64_FRAME_US);
    c64_exec(&c64, C64_FRAME_US);
    c64_key_up(&c64, ascii);
    c64_exec(&c64, C64_FRAME_US);
}

static bool init(void)
{
    odroid_system_init(APPID_GB, C64_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL, NULL, NULL, NULL);

    c64_desc_t desc = {0};
    desc.joystick_type       = C64_JOYSTICKTYPE_DIGITAL_12;  /* route input to both ports */
    desc.audio.callback.func = audio_cb;
    desc.audio.num_samples   = 128;
    desc.audio.sample_rate   = C64_AUDIO_SAMPLE_RATE;

    if (!load_bios(&desc)) return false;
    c64_init(&c64, &desc);
    c64_build_palette();

    /* warm up to the BASIC READY. prompt */
    for (int i = 0; i < 120; i++) c64_exec(&c64, C64_FRAME_US);

    /* Load the .prg (cached to flash) and autostart it with RUN<Return>. */
    uint32_t gsz = 0;
    const uint8_t *g = (const uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &gsz, false);
    if (g && gsz) {
        bool ok = c64_quickload(&c64, (chips_range_t){ .ptr = (void *)g, .size = gsz });
        printf("[C64] quickload %s -> %d (%u bytes)\n", ACTIVE_FILE->path, ok, (unsigned)gsz);
        if (ok) {
            const char *cmd = "RUN\r";
            for (const char *c = cmd; *c; c++) type_char((uint8_t)*c);
        }
    }
    return true;
}

void app_main_c64(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    odroid_gamepad_state_t joystick;

    if (!init()) {
        /* missing ROMs: paint one frame so the failure is visible, then idle. */
        c64_blit();
        lcd_swap();
        while (true) { wdog_refresh(); }
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);

    while (true) {
        wdog_refresh();
        common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, NULL);
        common_emu_input_loop_handle_turbo(&joystick);

        uint8_t m = 0;
        if (joystick.values[ODROID_INPUT_LEFT])  m |= C64_JOYSTICK_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT]) m |= C64_JOYSTICK_RIGHT;
        if (joystick.values[ODROID_INPUT_UP])    m |= C64_JOYSTICK_UP;
        if (joystick.values[ODROID_INPUT_DOWN])  m |= C64_JOYSTICK_DOWN;
        if (joystick.values[ODROID_INPUT_A])     m |= C64_JOYSTICK_BTN;
        c64_joystick(&c64, m, m);
        /* START -> Return (menu/start in many games and BASIC). */
        if (joystick.values[ODROID_INPUT_START]) c64_key_down(&c64, 0x0D);
        else                                     c64_key_up(&c64, 0x0D);

        c64_exec(&c64, C64_FRAME_US);
        c64_blit();
        lcd_swap();

        common_emu_sound_sync(false);
    }
}
