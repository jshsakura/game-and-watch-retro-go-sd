#include <odroid_system.h>

#include <string.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "bilinear.h"
#include "rg_i18n.h"

/* gpsp. The core's own headers pull in libretro types and register-name macros
 * that collide with CMSIS, so we declare the handful of entry points we use. */
#include "gba_savestate_abi.h"

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
 * renders a 240x160 image that then has to be scaled, so it needs a source. */
static uint16_t gba_framebuffer[GBA_WIDTH * GBA_HEIGHT];

static odroid_video_frame_t video_frame = {GBA_WIDTH, GBA_HEIGHT, GBA_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static void blit_emulator(void);

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

    init_main();
    init_memory();
    init_sound();

    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));

    /* The ROM is up to 32MB and stays in external flash, memory-mapped. Nothing
     * is copied into RAM and nothing is paged: the core reads the cart where it
     * lies. Page 0 is the exception — an RTC cart has its GPIO registers *written*
     * into "ROM" at 0x080000C4, and flash does not take writes, so the core keeps
     * a RAM shadow of it. Ruby, Sapphire and Emerald all keep time that way. */
    uint32_t rom_size = 0;
    uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &rom_size, false);
    if (rom == NULL || rom_size == 0) {
        odroid_overlay_alert(curr_lang->s_LoadFailed);
        odroid_system_switch_app(0);
    }
    gba_set_xip_rom(rom, rom_size);
    init_gamepak_buffer();

    if (load_gamepak(NULL, ACTIVE_FILE->path, 0, 0, 0) != 0) {
        odroid_overlay_alert(curr_lang->s_LoadFailed);
        odroid_system_switch_app(0);
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
