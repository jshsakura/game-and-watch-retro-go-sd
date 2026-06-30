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
#include "gw_audio.h"
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
    /* Scale the 200x160 picture to FILL the 320x240 LCD (nearest-neighbour). Fills
     * every pixel so no per-frame full-screen memset (faster + no black border). */
    static int16_t sx[WIDTH];
    static int started;
    if (!started) { for (int x = 0; x < WIDTH; x++) sx[x] = (int16_t)(x * GAMECOM_W / WIDTH); started = 1; }
    /* ACTIVE buffer — same one common_ingame_overlay() and pce/wsv/videopac draw
     * into, and the buffer lcd_swap() presents. The old inactive-buffer blit put
     * the game frame on a DIFFERENT buffer than the overlay, so the volume/
     * brightness/save popups landed on the not-shown buffer and "hid behind". */
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    for (int y = 0; y < 240; y++) {
        const uint8_t *src = &gamecom_fb[(y * GAMECOM_H / 240) * GAMECOM_W];
        uint16_t *dst = &out[y * WIDTH];
        for (int x = 0; x < WIDTH; x++) {
            uint8_t v = src[sx[x]];
            dst[x] = gc_pal565[v <= 4 ? v : 4];
        }
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

/* Savestate-slot thumbnail: hand retro-go the current rendered frame. */
static void *Screenshot(void)
{
    gc_blit();
    return lcd_get_active_buffer();
}

/* Battery NVRAM: persist the 8KB cartridge save (high scores, PDA data) to a
 * .sav so it survives power-off, like a real game.com's internal memory. */
static void LoadSram(void)
{
    char *p = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    FILE *f = p ? fopen(p, "rb") : NULL;
    if (f) {
        uint32_t n; uint8_t *nv = gamecom_nvram(&n);
        if (fread(nv, 1, n, f) != n) { /* short/empty .sav: keep init value */ }
        fclose(f);
    }
    free(p);
}

static void SaveSram(void)
{
    char *p = odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path);
    FILE *f = p ? fopen(p, "wb") : NULL;
    if (f) {
        uint32_t n; uint8_t *nv = gamecom_nvram(&n);
        fwrite(nv, 1, n, f);
        fclose(f);
    }
    free(p);
}

/* Fill this frame's audio buffer from the core's SG0/SG1 + DAC mix, scaled by
 * the user volume (same factor>>8 convention as pce/wsv). */
static void gamecom_pcm_submit(void)
{
    int16_t *buf = audio_get_active_buffer();
    uint16_t len = audio_get_buffer_length();
    if (common_emu_sound_loop_is_muted()) {
        memset(buf, 0, len * sizeof(int16_t));
        return;
    }
    int32_t factor = common_emu_sound_get_volume();
    gamecom_audio_mix(buf, len, GAMECOM_AUDIO_SAMPLE_RATE);
    for (int i = 0; i < len; i++) {
        int32_t s = ((int32_t)buf[i] * factor) >> 8;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
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
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, &SaveSram);
    /* Start the audio DMA: the per-frame common_emu_sound_sync busy-waits for the DMA
     * counter, so without this the first frame hangs forever (no audio = no DMA tick). */
    audio_start_playing(GAMECOM_AUDIO_SAMPLE_RATE / 60);

    /* firmware /bios/<sys>/ convention; copyright Tiger ROMs, user-supplied. */
    uint32_t isz = 0, ksz = 0, csz = 0;

    /* Cache the (up to 2MB) cartridge FIRST. The circular flash cache invalidates
     * earlier files when a large write wraps, so caching the small, every-launch
     * BIOS ROMs LAST stops the big per-game cart from clobbering them — a clobbered
     * external BIOS leaves the SM8500 running garbage = the instant freeze on launch.
     * Cart is XIP'd in place (never copied to RAM). */
    const uint8_t *cart = (const uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &csz, false);
    const uint8_t *irom = cache_rom("/bios/gamecom/internal.bin", 0x1000,  &isz);
    const uint8_t *krom = cache_rom("/bios/gamecom/external.bin", 0x40000, &ksz);
    if (!irom || !krom) return false;

    if (gamecom_init(irom, (int)isz, krom, (int)ksz, cart, (int)csz) != 0) {
        printf("[GAMECOM] gamecom_init failed\n");
        return false;
    }
    gc_build_palette();
    LoadSram();   /* restore the cartridge NVRAM save (after gamecom_init zeroed it) */
    return true;
}

void app_main_gamecom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    odroid_gamepad_state_t joystick;

    if (!init()) {
        /* missing ROMs / bad cart: bounce back to the launcher instead of freezing in a
         * wdog-fed loop (which also blocked the watchdog from recovering). Like other cores. */
        return;
    }

    if (load_state)
        odroid_system_emu_load_state(save_slot);

    int frame = 0, skips = 0;

    while (true) {
        wdog_refresh();
        /* PCE/videopac pacing: decouple audio from video. When we fall behind,
         * common_emu_frame_loop() returns false -> skip only the 320x240 blit to
         * catch up, but still emulate + refill the audio buffer every loop so the
         * music never underruns (the "득득" stutter). Guard: never skip more than
         * 2 in a row, so the screen can't visually freeze. */
        bool draw = common_emu_frame_loop();
        if (!draw && ++skips >= 3) draw = true;
        if (draw) skips = 0;

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &gc_blit);   /* repaint cb: NULL -> pause menu called (*NULL)() = PC=0 HardFault */
        common_emu_input_loop_handle_turbo(&joystick);

        /* G&W buttons -> game.com ports (active low: 0 bit = pressed). game.com has 4
         * action buttons A/B/C/D; map ALL of them (the old map wasted START/SELECT on the
         * system MENU/PAUSE — which retro-go's own overlay already covers — and left C/D
         * dead, so games/menus needing C or D couldn't be operated). */
        uint8_t in0 = 0xFF, in1 = 0xFF, in2 = 0xFF;
        if (joystick.values[ODROID_INPUT_UP])     in0 &= ~GC_IN0_UP;
        if (joystick.values[ODROID_INPUT_DOWN])   in0 &= ~GC_IN0_DOWN;
        if (joystick.values[ODROID_INPUT_LEFT])   in0 &= ~GC_IN0_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT])  in0 &= ~GC_IN0_RIGHT;
        if (joystick.values[ODROID_INPUT_A])      in0 &= ~GC_IN0_A;   /* A */
        if (joystick.values[ODROID_INPUT_B])      in1 &= ~GC_IN1_B;   /* B */
        if (joystick.values[ODROID_INPUT_START])  in1 &= ~GC_IN1_C;   /* START  -> C */
        if (joystick.values[ODROID_INPUT_SELECT]) in2 &= ~GC_IN2_D;   /* SELECT -> D */
        gamecom_set_input_state(in0, in1, in2);

        /* Stylus bridge (see file header). */
        if (!load_state && frame >= GC_LAUNCH_BEGIN && frame < GC_LAUNCH_END)
            gamecom_set_stylus(45, 60, 1);                 /* auto-enter CARTRIDGE */
        else if (joystick.values[ODROID_INPUT_A])
            gamecom_set_stylus(GAMECOM_W / 2, GAMECOM_H / 2, 1); /* A = centre tap */
        else
            gamecom_set_stylus(0, 0, 0);

        gamecom_run_frame();
        if (draw) {
            gc_blit();
            common_ingame_overlay();     /* draw volume/brightness/pause overlay ON the frame */
            lcd_swap();
        }

        gamecom_pcm_submit();            /* audio EVERY loop, drawn or skipped */
        common_emu_sound_sync(false);
        frame++;
    }
}
