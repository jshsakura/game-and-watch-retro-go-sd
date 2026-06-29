/* ZX Spectrum 48K porting layer (floooh/chips zx.h core).
 * Video+input first; audio is a no-op callback for v1 (added later, like PCE-CD
 * shipping before its CD audio). Mirrors the videopac/pce porting pattern. */
#include <odroid_system.h>
#include <string.h>

#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include "rom_manager.h"
#include "main_zx.h"

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/zx.h"

#define ZX_AUDIO_SAMPLE_RATE 22050
#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
/* LCD is 320x240; the ZX visible field is 320x256 (border baked in) — drop 8
 * scanlines top & bottom to centre it. */
#define ZX_CROP_TOP 8

static zx_t      zx;
static uint16_t  zx_pal565[16];
static bool      zx_is128;

static void audio_cb(const float *s, int n, void *u) { (void)s; (void)n; (void)u; }

/* ---- save/load: dump the live zx_t straight to disk. It's a static at a fixed
 * address, so all its internal self-pointers (mem page table -> zx.ram/rom) and
 * the audio callback ptr stay valid after reload within the same firmware build.
 * Avoids the chips snapshot API, whose zx_load_snapshot keeps a 322KB static
 * scratch zx_t that would blow the RAM_EMU overlay budget. ---- */
static bool SaveState(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&zx, sizeof(zx), 1, f);
    fclose(f);
    return true;
}

static bool LoadState(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fread(&zx, sizeof(zx), 1, f);
    fclose(f);
    return true;
}

static void zx_build_palette(void)
{
    chips_display_info_t di = zx_display_info(&zx);
    const uint32_t *pal = (const uint32_t *)di.palette.ptr;   /* 0xAABBGGRR */
    for (int i = 0; i < 16; i++) {
        uint32_t c = pal[i];
        zx_pal565[i] = RGB565(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
    }
}

static void zx_blit(void)
{
    uint16_t *out = (uint16_t *)lcd_get_inactive_buffer();
    for (int y = 0; y < 240; y++) {
        const uint8_t *src = &zx.fb[(y + ZX_CROP_TOP) * ZX_FRAMEBUFFER_WIDTH];
        uint16_t *dst = &out[y * WIDTH];
        for (int x = 0; x < ZX_DISPLAY_WIDTH; x++)
            dst[x] = zx_pal565[src[x] & 15];
    }
}

static bool load_bios(zx_desc_t *desc)
{
    uint32_t sz = 0;
    /* 48K only for v1: read the 16K ROM straight off the SD (firmware /bios/<sys>/
     * convention; cached in flash, zx_init memcpy's it into core RAM). */
    const uint8_t *p = (const uint8_t *)odroid_overlay_cache_file_in_flash("/bios/zx/48.rom", &sz, false);
    if (!p || sz < 0x4000) {
        printf("[ZX] missing /bios/zx/48.rom (got %u)\n", (unsigned)sz);
        return false;
    }
    desc->roms.zx48k.ptr  = (void *)p;
    desc->roms.zx48k.size = 0x4000;
    return true;
}

static void init(void)
{
    odroid_system_init(APPID_GB, ZX_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL, NULL, NULL, NULL);

    zx_desc_t desc = {0};
    desc.type           = ZX_TYPE_48K;
    desc.joystick_type  = ZX_JOYSTICKTYPE_KEMPSTON;
    desc.audio.callback.func = audio_cb;
    desc.audio.num_samples   = 128;
    desc.audio.sample_rate   = ZX_AUDIO_SAMPLE_RATE;
    desc.audio.beeper_volume = 0.5f;
    desc.audio.ay_volume     = 0.5f;
    zx_is128 = false;

    if (!load_bios(&desc)) return;
    zx_init(&zx, &desc);
    zx_build_palette();

    /* Load the .z80 game (cached to flash, parsed read-only by zx_quickload). */
    uint32_t gsz = 0;
    const uint8_t *g = (const uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &gsz, false);
    if (g && gsz) {
        bool ok = zx_quickload(&zx, (chips_range_t){ .ptr = (void *)g, .size = gsz });
        printf("[ZX] quickload %s -> %d (%u bytes)\n", ACTIVE_FILE->path, ok, (unsigned)gsz);
    }
}

void app_main_zx(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    odroid_gamepad_state_t joystick;

    init();

    if (load_state)
        odroid_system_emu_load_state(save_slot);

    while (true) {
        wdog_refresh();
        common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, NULL);
        common_emu_input_loop_handle_turbo(&joystick);

        uint8_t m = 0;
        if (joystick.values[ODROID_INPUT_LEFT])  m |= ZX_JOYSTICK_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT]) m |= ZX_JOYSTICK_RIGHT;
        if (joystick.values[ODROID_INPUT_UP])    m |= ZX_JOYSTICK_UP;
        if (joystick.values[ODROID_INPUT_DOWN])  m |= ZX_JOYSTICK_DOWN;
        if (joystick.values[ODROID_INPUT_A])     m |= ZX_JOYSTICK_BTN;
        zx_joystick(&zx, m);
        /* START -> Enter (menu/start in many games) */
        if (joystick.values[ODROID_INPUT_START]) zx_key_down(&zx, 0x0D);
        else                                     zx_key_up(&zx, 0x0D);

        zx_exec(&zx, 19968);     /* one 50Hz PAL frame */
        zx_blit();
        lcd_swap();

        common_emu_sound_sync(false);
    }
}
