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

/* oswan (WonderSwan / Color) core. We forward-declare the handful of entry
 * points we use instead of including WS.h, which exposes generic globals
 * (IO, Page, ...) that risk clashing with the firmware / CMSIS headers. */
void WsInit(void);
void WsReset(void);
uint32_t WsRun(void);
extern uint16_t FrameBuffer[240 * 144];          /* render target (NOSDL_FB) */
int  ws_create_from_flash(const uint8_t *data, uint32_t size); /* G&W ROM loader */

/* APU ring buffer (WSApu.c). apuBufGetLock/Unlock live in the excluded SDL
 * backend, so we drain the ring directly. */
#define WS_SND_RNGSIZE (32 * 512)
extern int16_t sndbuffer[2][WS_SND_RNGSIZE];
extern int32_t rBuf, wBuf;

/* Referenced by oswan's WsLoadEeprom (defined in the excluded SDL front-end). */
char gameName[512];

/* End-of-frame present hook. oswan's Interrupt() calls graphics_paint() at
 * vblank; the SDL front-end blits there. We blit FrameBuffer ourselves in the
 * app loop, so this is a no-op. Namespaced (ws__graphics_paint via
 * wswan_redefines) so it does not collide with RACE's graphics_paint. */
void graphics_paint(void) { }

/* WonderSwan native screen: 224x144, rendered into FrameBuffer at row stride
 * 240 with an 8-pixel left margin (see RefreshLine). */
#define WIDTH       320
#define WS_WIDTH    (224)
#define WS_HEIGHT   (144)
#define WS_STRIDE   (240)
#define WS_XOFF     (8)
#define WS_FPS      (75)   /* WonderSwan refresh ~75.47 Hz */

#define WS_SAMPLE_RATE         (44100)
#define WS_AUDIO_BUFFER_LENGTH (WS_SAMPLE_RATE / WS_FPS)
static int16_t audioBuffer_ws[WS_AUDIO_BUFFER_LENGTH * 2];

static odroid_video_frame_t video_frame = {WS_WIDTH, WS_HEIGHT, WS_STRIDE * 2, 2, 0xFF, -1, NULL, NULL, 0, {}};

/* The G&W input register oswan reads via WsInputGetState(). Bit layout (see
 * WS.c): Y1-4 = bits 0-3, X1-4 (D-pad) = bits 4-7, OPTION=8, START=9, A=10,
 * B=11. We drive the X-pad as the main D-pad. */
uint32_t WsInputGetState(void)
{
    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);
    uint32_t s = 0;
    /* Drive the D-pad on BOTH the X-pad (bits 4-7) and Y-pad (bits 0-3) so it
     * works whether the game reads the horizontal or vertical pad. */
    if (joystick.values[ODROID_INPUT_UP])     s |= 0x0010 | 0x0001; /* X1 | Y1 */
    if (joystick.values[ODROID_INPUT_RIGHT])  s |= 0x0020 | 0x0002; /* X2 | Y2 */
    if (joystick.values[ODROID_INPUT_DOWN])   s |= 0x0040 | 0x0004; /* X3 | Y3 */
    if (joystick.values[ODROID_INPUT_LEFT])   s |= 0x0080 | 0x0008; /* X4 | Y4 */
    if (joystick.values[ODROID_INPUT_A])      s |= 0x0400; /* WS A = red A */
    /* WS B on the B button AND the GAME button (the bare B isn't reachable on
     * some G&W units), so 2-button games always have a working B. */
    if (joystick.values[ODROID_INPUT_B] ||
        joystick.values[ODROID_INPUT_START])  s |= 0x0800; /* WS B (bit11) */
    if (joystick.values[ODROID_INPUT_SELECT]) s |= 0x0200; /* WS START = TIME */
    if (joystick.values[ODROID_INPUT_X])      s |= 0x0100; /* WS OPTION */
    return s;
}

static void blit_emulator(void);

/* Savestates are not wired for v1 (oswan's WsSaveState is FILE-based). */
static bool LoadState(const char *savePathName) { (void)savePathName; return false; }
static bool SaveState(const char *savePathName) { (void)savePathName; return false; }

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    blit_emulator();
    return lcd_get_active_buffer();
}

void ws_pcm_submit(void)
{
    int16_t *dst = audio_get_active_buffer();
    uint16_t dst_len = audio_get_buffer_length();
    if (common_emu_sound_loop_is_muted()) {
        return;
    }
    int32_t factor = common_emu_sound_get_volume();
    /* Drain the APU stereo ring, mixing L+R down to the mono G&W output. */
    for (int i = 0; i < dst_len; i++) {
        int16_t s = 0;
        if (rBuf != wBuf) {
            s = (int16_t)((sndbuffer[0][rBuf] + sndbuffer[1][rBuf]) >> 1);
            if (++rBuf >= WS_SND_RNGSIZE) rBuf = 0;
        }
        dst[i] = s * factor;
    }
}

__attribute__((optimize("unroll-loops")))
static inline void screen_blit_nn(int32_t dest_width, int32_t dest_height)
{
    int w1 = WS_WIDTH;
    int h1 = WS_HEIGHT;
    int w2 = dest_width;
    int h2 = dest_height;

    int x_ratio = (int)((w1 << 16) / w2) + 1;
    int y_ratio = (int)((h1 << 16) / h2) + 1;
    int hpad = (320 - dest_width) / 2;
    int wpad = (240 - dest_height) / 2;

    uint16_t *dest = lcd_get_active_buffer();

    for (int i = 0; i < h2; i++) {
        for (int j = 0; j < w2; j++) {
            int x2 = ((j * x_ratio) >> 16);
            int y2 = ((i * y_ratio) >> 16);
            /* source row stride is WS_STRIDE with an 8px left margin */
            dest[((i + wpad) * WIDTH) + j + hpad] = FrameBuffer[(y2 * WS_STRIDE) + x2 + WS_XOFF];
        }
    }
}

static void screen_blit_bilinear(int32_t dest_width)
{
    int hpad = (320 - dest_width) / 2;
    uint16_t *dest = lcd_get_active_buffer();

    image_t dst_img = { dest_width, 240, 2, (uint8_t *)dest + hpad * 2 };
    image_t src_img = { WS_WIDTH, WS_HEIGHT, 2, (uint8_t *)(FrameBuffer + WS_XOFF) };
    /* NOTE: imlib uses a packed source; WS_STRIDE margin handled via WS_XOFF
     * base. Bilinear is best-effort; nearest is the crisp default. */

    if (hpad > 0) {
        memset(dest, 0x00, hpad * 2);
    }
    float x_scale = ((float)dest_width) / ((float)WS_WIDTH);
    float y_scale = 240.0f / ((float)WS_HEIGHT);
    imlib_draw_image(&dst_img, &src_img, 0, 0, 320, x_scale, y_scale, NULL, -1, 255, NULL,
                     NULL, IMAGE_HINT_BILINEAR, NULL, NULL);
}

static void blit_emulator(void)
{
    odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();
    odroid_display_filter_t filtering = odroid_display_get_filter_mode();

    switch (scaling) {
    case ODROID_DISPLAY_SCALING_OFF:
        screen_blit_nn(WS_WIDTH, WS_HEIGHT);
        break;
    case ODROID_DISPLAY_SCALING_FIT:
        /* 224x144 -> full height 240, width 224*240/144 = 373 -> clamp 320 */
        screen_blit_nn(320, 240);
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
        screen_blit_nn(320, 240);
        break;
    }
}

static void blit(void) {
    blit_emulator();
    common_ingame_overlay();
}

static size_t ws_getromdata(unsigned char **data) {
    uint32_t size = 0;
    const unsigned char *src = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    if (src == NULL || size == 0) {
        *data = NULL;
        return 0;
    }
    *data = (unsigned char *)src;
    return size;
}

void app_main_wswan(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
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
    common_emu_state.frame_time_10us = (uint16_t)(100000 / WS_FPS + 0.5f);
    lcd_set_refresh_rate(WS_FPS);

    video_frame.buffer = FrameBuffer;

    odroid_system_init(APPID_WSWAN, WS_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL);

    /* No saved per-app scaling default (defaults to OFF = tiny native), so fill
     * the screen the first time instead. */
    if (odroid_display_get_scaling_mode() == ODROID_DISPLAY_SCALING_OFF)
        odroid_display_set_scaling_mode(ODROID_DISPLAY_SCALING_FULL);

    audio_start_playing(WS_AUDIO_BUFFER_LENGTH);

    WsInit();

    rom_length = ws_getromdata(&rom_ptr);
    ws_create_from_flash(rom_ptr, (uint32_t)rom_length);
    WsReset();

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

        WsRun();

        if (drawFrame) {
            blit();
            lcd_swap();
        }

        ws_pcm_submit();

        common_emu_sound_sync(false);
    }
}
