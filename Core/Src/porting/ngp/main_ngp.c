#include <odroid_system.h>

#include <assert.h>
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

/* RACE (Neo Geo Pocket / Color) core */
#include "graphics.h"
#include "state.h"

/* Forward-declare the core entry points we call. We deliberately avoid several
 * RACE headers because they pollute the namespace against the STM32 CMSIS
 * headers also included in this TU: main.h drags in <libretro.h>; tlcs900h.h
 * defines register-name macros (SP/PC/...); neopopsound.h uses an identifier
 * 'RNG' that collides with the CMSIS RNG peripheral macro. */
void mainemuinit(void);
int handleInputFile(const char *romName, const unsigned char *romData, int romSize);
void tlcs_execute(int cycles, int skipRender);
void system_sound_chipreset(int sample_rate);
void sound_update(uint16_t *chip_buffer, int length_bytes);
void dac_update(uint16_t *dac_buffer, int length_bytes);

/* NGPC input register. Upstream this is defined in the libretro front-end
 * (excluded here), so we own it; the core reads it via race-memory.h. */
uint8_t ngpInputState = 0;

/* Other front-end globals the core reads, also defined in the excluded
 * libretro front-end upstream: console-variant override, gfx hacks, BIOS
 * language. Defaults (0) keep auto-detection / English BIOS. */
int tipo_consola = 0;
int gfx_hacks = 0;
int setting_ngp_language = 0;

#define WIDTH 320

/* Native NGPC screen. FB_WIDTH/FB_HEIGHT in the RACE libretro front-end. */
#define NGP_WIDTH  (160)
#define NGP_HEIGHT (152)
#define NGP_FPS    (60)

/* The RACE main CPU clock; one frame = CPU_FREQ / 60 ticks. */
#define NGP_CPU_FREQ      (6144000)
#define NGP_CYCLES_FRAME  (NGP_CPU_FREQ / NGP_FPS)

/* Audio: NGPC sound is synthesised; we pull one frame of mono samples. */
#define NGP_SAMPLE_RATE        (44100)
#define NGP_AUDIO_BUFFER_LENGTH (NGP_SAMPLE_RATE / NGP_FPS)
static int16_t audioBuffer_ngp[NGP_AUDIO_BUFFER_LENGTH];

/* RGB565 framebuffer the RACE renderer draws straight into (screen->pixels). */
static uint16_t ngp_framebuffer[NGP_WIDTH * NGP_HEIGHT];

/* RACE expects the front-end to provide this screen object and graphics_paint. */
static struct ngp_screen ngp_screen_obj;
struct ngp_screen *screen = &ngp_screen_obj;

static odroid_video_frame_t video_frame = {NGP_WIDTH, NGP_HEIGHT, NGP_WIDTH * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

static void blit_emulator(void);

/* The RACE scanline renderer writes pixels into screen->pixels as the CPU
 * runs, so by the time a frame completes the framebuffer already holds the
 * image. graphics_paint() is just the present hook - nothing to copy here. */
void graphics_paint(unsigned char render)
{
    (void)render;
}

static bool LoadState(const char *savePathName) {
    /* Silence the SAI output buffers up front: the main loop is blocked during
     * the file read below, so the audio DMA would otherwise loop the last
     * buffer (a latched tone = the post-load beep) until playback resumes. */
    audio_clear_buffers();

    /* Stash the serialised state in the off-screen framebuffer. */
    unsigned char *data = (unsigned char *)lcd_get_active_buffer();
    int size = state_get_size();

    FILE *file = fopen(savePathName, "rb");
    if (file == NULL) {
        return false;
    }

    size_t read = fread(data, size, 1, file);
    fclose(file);
    if (!read) {
        return false;
    }

    state_restore_mem(data);
    lcd_clear_active_buffer();
    return true;
}

static bool SaveState(const char *savePathName) {
    lcd_wait_for_vblank();
    unsigned char *data = (unsigned char *)lcd_get_active_buffer();
    int size = state_get_size();

    state_store_mem(data);

    FILE *file = fopen(savePathName, "wb");
    if (file == NULL) {
        return false;
    }

    size_t written = fwrite(data, size, 1, file);
    fclose(file);
    return written != 0;
}

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

void ngp_pcm_submit(void) {
    /* RACE writes mono int16 samples for one frame; sound_update lays down the
     * tone channels and dac_update mixes the DAC channel on top. */
    int samples = NGP_AUDIO_BUFFER_LENGTH;
    sound_update((uint16_t *)audioBuffer_ngp, samples * sizeof(int16_t));
    dac_update((uint16_t *)audioBuffer_ngp, samples * sizeof(int16_t));

    if (common_emu_sound_loop_is_muted()) {
        return;
    }

    int32_t factor = common_emu_sound_get_volume();
    int16_t *sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();

    for (int i = 0; i < sound_buffer_length; i++) {
        /* audioBuffer_ngp is full-scale int16; factor is 0-255 (volume_tbl),
         * so scale by factor/256 to apply volume and stay within int16. */
        sound_buffer[i] = (int16_t)((audioBuffer_ngp[i] * factor) >> 8);
    }
}

__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    uint16_t *screen_buf = (uint16_t *)video_frame.buffer;
    uint16_t *dest = lcd_get_active_buffer();

    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            int x2 = ((j * x_ratio) >> 16);
            int y2 = ((i * y_ratio) >> 16);
            dest[((i + wpad) * WIDTH) + j + hpad] = screen_buf[(y2 * w1) + x2];
        }
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int w1 = video_frame.width;
    int h1 = video_frame.height;
    int w2 = dest_width;
    int h2 = 240;
    int stride = 320;
    int hpad = (320 - dest_width) / 2;

    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img;
    dst_img.w = dest_width;
    dst_img.h = 240;
    dst_img.bpp = 2;
    dst_img.pixels = ((uint8_t *)dest) + hpad * 2;

    if (hpad > 0) {
        memset(dest, 0x00, hpad * 2);
    }

    image_t src_img;
    src_img.w = video_frame.width;
    src_img.h = video_frame.height;
    src_img.bpp = 2;
    src_img.pixels = video_frame.buffer;

    float x_scale = ((float)w2) / ((float)w1);
    float y_scale = ((float)h2) / ((float)h1);

    imlib_draw_image(&dst_img, &src_img, 0, 0, stride, x_scale, y_scale, NULL, -1, 255, NULL,
                     NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

static void blit_emulator(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    /* 160x152: full height is 240 -> width 160*240/152 = 252 (4:3-ish). */
    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        /* 160x152 centred -> black border; clear so a previous wider frame's
         * pixels don't remain as garbage around the image. */
        lcd_clear_active_buffer();
        screen_blit_nn(NGP_WIDTH, NGP_HEIGHT);
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        /* Aspect-preserving 252x240 -> left/right pillarbox bars; clear them so
         * they aren't stale pixels from a previous full-screen frame. */
        lcd_clear_active_buffer();
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            screen_blit_bilinear(252);
        } else {
            screen_blit_nn(252, 240);
        }
        break;
    case ODROID_DISPLAY_SCALING_FULL:
    case ODROID_DISPLAY_SCALING_CUSTOM:
        if (filtering == ODROID_DISPLAY_FILTER_SOFT) {
            screen_blit_bilinear(320);
        } else {
            screen_blit_nn(320, 240);
        }
        break;
    default:
        screen_blit_nn(252, 240);
        break;
    }
}

static void blit(void) {
    blit_emulator();
    common_ingame_overlay();
}

static void ngp_input_read(odroid_gamepad_state_t *joystick) {
    uint8_t state = 0x00;
    if (joystick->values[ODROID_INPUT_UP])     state |= 0x01;
    if (joystick->values[ODROID_INPUT_DOWN])   state |= 0x02;
    if (joystick->values[ODROID_INPUT_LEFT])   state |= 0x04;
    if (joystick->values[ODROID_INPUT_RIGHT])  state |= 0x08;
    if (joystick->values[ODROID_INPUT_A])      state |= 0x10; /* NGP A = red A */
    if (joystick->values[ODROID_INPUT_B])      state |= 0x20; /* NGP B = B button */
    if (joystick->values[ODROID_INPUT_START]  || joystick->values[ODROID_INPUT_SELECT] ||
        joystick->values[ODROID_INPUT_X]      || joystick->values[ODROID_INPUT_Y])
        state |= 0x40; /* NGP Option = GAME/TIME/START/SELECT */
    ngpInputState = state;
}

/* NGPC ROMs are large (up to 4 MiB) and read-only during play, so we leave
 * them memory-mapped in external flash (XIP) and hand RACE a pointer instead
 * of copying into RAM - the house pattern (see PCE.ROM). Cart-flash saves are
 * handled by the core's RAM shadow, not by writing this buffer. */
static size_t ngp_getromdata(unsigned char **data) {
    uint32_t size = 0;
    const unsigned char *src = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    if (src == NULL || size == 0) {
        *data = NULL;
        return 0;
    }
    *data = (unsigned char *)src;
    return size;
}

void app_main_ngp(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    uint32_t rom_length = 0;
    unsigned char *rom_ptr = NULL;
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
    common_emu_state.frame_time_10us = (uint16_t)(100000 / NGP_FPS + 0.5f);
    lcd_set_refresh_rate(NGP_FPS);

    video_frame.buffer = ngp_framebuffer;

    /* Point the RACE renderer at our RGB565 framebuffer. */
    screen->w = NGP_WIDTH;
    screen->h = NGP_HEIGHT;
    screen->pixels = ngp_framebuffer;

    odroid_system_init(APPID_NGP, NGP_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL);

    /* First-run default = FIT (global OFF = tiny native, too small here). FIT
     * fills the screen while preserving aspect — sensible, without FULL's stretch.
     * Only the factory-default OFF is bumped; any user choice (FULL/FIT/...) is
     * saved to the shared global setting and respected afterwards. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FIT);

    audio_start_playing(NGP_AUDIO_BUFFER_LENGTH);

    /* Init order mirrors the RACE front-end: sound chip, emulator, then ROM. */
    system_sound_chipreset(NGP_SAMPLE_RATE);
    mainemuinit();

    rom_length = ngp_getromdata(&rom_ptr);
    handleInputFile(ACTIVE_FILE->path, rom_ptr, (int)rom_length);

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    while (1)
    {
        wdog_refresh();

        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &blit);
        common_emu_input_loop_handle_turbo(&joystick);

        ngp_input_read(&joystick);

        tlcs_execute(NGP_CYCLES_FRAME, drawFrame ? 0 : 1);

        if (drawFrame) {
            blit();
            lcd_swap();
        }

        ngp_pcm_submit();

        common_emu_sound_sync(false);
    }
}
