/* Tiger Game.com porting layer (Sharp SM8500/SM8521 core, ported from MAME).
 * Video + button input + auto-launch first; audio (SG0/SG1/DAC) is a no-op for
 * v1, like ZX/C64/PCE-CD shipping before their audio. Mirrors the zx/c64 pattern.
 *
 * The Game & Watch has no touchscreen, but the game.com PDA menu and many in-game
 * prompts are stylus-only. We bridge that without burning a button:
 *   - on a fresh boot we auto-tap the CARTRIDGE icon so the inserted cart launches
 *   - the A button doubles as a centre-screen tap, so "tap to continue" prompts
 *     pass while A still acts as game.com button A (action games poll buttons). */
#include <odroid_system.h>
#include <string.h>

#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include "rom_manager.h"
#include "main_gamecom.h"

#include "gamecom_core.h"
#include "sm8500.h"

#define GAMECOM_AUDIO_SAMPLE_RATE 22050
#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

/* 200x160 core picture centred on the 320x240 LCD. */
#define GC_X0 ((WIDTH  - GAMECOM_W) / 2)   /* 60 */
#define GC_Y0 ((240    - GAMECOM_H) / 2)   /* 40 */

/* Auto-launch window: after the boot animation the PDA menu becomes interactive
 * around frame ~400; hold a tap on the CARTRIDGE icon through this window so the
 * cart enters without a touchscreen (matches the host-harness launch sequence). */
#define GC_LAUNCH_BEGIN 400
#define GC_LAUNCH_END   520

static uint16_t gc_pal565[5];

static void gc_build_palette(void)
{
    for (int i = 0; i < 5; i++)
        gc_pal565[i] = RGB565(gamecom_palette[i][0], gamecom_palette[i][1], gamecom_palette[i][2]);
}

static void gc_blit(void)
{
    uint16_t *out = (uint16_t *)lcd_get_inactive_buffer();
    memset(out, 0, WIDTH * 240 * sizeof(uint16_t));   /* letterbox border */
    for (int y = 0; y < GAMECOM_H; y++) {
        const uint8_t *src = &gamecom_fb[y * GAMECOM_W];
        uint16_t *dst = &out[(y + GC_Y0) * WIDTH + GC_X0];
        for (int x = 0; x < GAMECOM_W; x++)
            dst[x] = gc_pal565[src[x] <= 4 ? src[x] : 4];
    }
}

/* ---- save/load: stream all machine + CPU state to/from the slot file. ---- */
static int rw_write(void *ctx, void *data, uint32_t len) { return fwrite(data, 1, len, (FILE *)ctx) == len; }
static int rw_read (void *ctx, void *data, uint32_t len) { return fread (data, 1, len, (FILE *)ctx) == len; }

static bool SaveState(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int ok = gamecom_state_rw(rw_write, f, 1);
    fclose(f);
    return ok != 0;
}

static bool LoadState(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    int ok = gamecom_state_rw(rw_read, f, 0);
    fclose(f);
    return ok != 0;
}

static const uint8_t *cache_rom(const char *path, uint32_t want, uint32_t *sz_out)
{
    uint32_t sz = 0;
    const uint8_t *p = (const uint8_t *)odroid_overlay_cache_file_in_flash(path, &sz, false);
    if (!p || sz < want) {
        printf("[GAMECOM] missing %s (got %u, want %u)\n", path, (unsigned)sz, (unsigned)want);
        if (sz_out) *sz_out = 0;
        return NULL;
    }
    if (sz_out) *sz_out = sz;
    return p;
}

static bool init(void)
{
    odroid_system_init(APPID_GB, GAMECOM_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL, NULL, NULL, NULL);

    /* firmware /bios/<sys>/ convention; copyright Tiger ROMs, user-supplied. */
    uint32_t isz = 0, ksz = 0, csz = 0;
    const uint8_t *irom = cache_rom("/bios/gamecom/internal.bin", 0x1000,  &isz);
    const uint8_t *krom = cache_rom("/bios/gamecom/external.bin", 0x40000, &ksz);
    if (!irom || !krom) return false;

    /* cartridge: flash-XIP in place (up to 2MB, never copied to RAM). */
    const uint8_t *cart = (const uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &csz, false);

    if (gamecom_init(irom, (int)isz, krom, (int)ksz, cart, (int)csz) != 0) {
        printf("[GAMECOM] gamecom_init failed\n");
        return false;
    }
    gc_build_palette();
    return true;
}

void app_main_gamecom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    odroid_gamepad_state_t joystick;

    if (!init()) {
        /* missing ROMs / bad cart: paint one (black) frame so it's visible, idle. */
        gc_blit();
        lcd_swap();
        while (true) { wdog_refresh(); }
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);

    int frame = 0;

    while (true) {
        wdog_refresh();
        common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, NULL);
        common_emu_input_loop_handle_turbo(&joystick);

        /* G&W buttons -> game.com ports (active low: 0 bit = pressed). */
        uint8_t in0 = 0xFF, in1 = 0xFF;
        if (joystick.values[ODROID_INPUT_UP])    in0 &= ~GC_IN0_UP;
        if (joystick.values[ODROID_INPUT_DOWN])  in0 &= ~GC_IN0_DOWN;
        if (joystick.values[ODROID_INPUT_LEFT])  in0 &= ~GC_IN0_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT]) in0 &= ~GC_IN0_RIGHT;
        if (joystick.values[ODROID_INPUT_A])     in0 &= ~GC_IN0_A;
        if (joystick.values[ODROID_INPUT_B])     in1 &= ~GC_IN1_B;
        if (joystick.values[ODROID_INPUT_START]) in0 &= ~GC_IN0_MENU;   /* START -> Menu */
        if (joystick.values[ODROID_INPUT_SELECT]) in0 &= ~GC_IN0_PAUSE; /* SELECT -> Pause */
        gamecom_set_input_state(in0, in1, 0xFF);

        /* Stylus bridge (see file header). */
        if (!load_state && frame >= GC_LAUNCH_BEGIN && frame < GC_LAUNCH_END)
            gamecom_set_stylus(45, 60, 1);                 /* auto-enter CARTRIDGE */
        else if (joystick.values[ODROID_INPUT_A])
            gamecom_set_stylus(GAMECOM_W / 2, GAMECOM_H / 2, 1); /* A = centre tap */
        else
            gamecom_set_stylus(0, 0, 0);

        gamecom_run_frame();
        gc_blit();
        lcd_swap();

        common_emu_sound_sync(false);
        frame++;
    }
}
