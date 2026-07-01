/*
 * Nintendo Virtual Boy — retro-go firmware glue for the red-viper core.
 *
 * RAM-overlay core (like a7800): loaded into __RAM_EMU_START__, runs its own
 * frame loop here. Interpreter-only (the red-viper DRC is ARM32 codegen; the
 * M7 is Thumb-2). Device specifics live behind -DGNW_VB_DEVICE in the core:
 *   - ROM XIP'd from QSPI flash with a rom_size-1 mirror mask (no 16MB buffer)
 *   - single-player RAM (338KB) from the RAM_EMU heap via vb_dev_calloc/malloc
 *
 * Save/load use the verified Core/Src/porting/vb/vb_savestate.c module
 * (round-trip + truncation + CRC guard proven on the host harness).
 */

#include <odroid_system.h>
#include <string.h>

#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "gw_malloc.h"
#include "rom_manager.h"
#include "common.h"
#include "appid.h"

/* red-viper core */
#include "v810_cpu.h"
#include "v810_mem.h"
#include "vb_set.h"
#include "vb_dsp.h"
#include "vb_sound.h"

/* ---- device platform layer the core expects (harness stubs these) -------- */

/* The ~338KB of RAM regions + ~15KB of wave buffers come from the RAM_EMU bump
 * heap, NOT newlib malloc. ram_start must already point past the VB overlay BSS
 * (set in getromdata) before v810_init() runs. */
void *vb_dev_calloc(size_t nmemb, size_t size) { return ram_calloc(nmemb, size); }
void *vb_dev_malloc(size_t size)               { return ram_malloc(size); }

/* tDSPCACHE normally lives in the GLES2 renderer video.c, which we don't build;
 * the software renderer (video_soft.cpp) uses it, so define it here. */
VB_DSPCACHE tDSPCACHE;

/* VIP framebuffer download is a hard-GL-renderer concern; the software path
 * composites straight into V810_DISPLAY_RAM, so this is a no-op on device. */
void video_download_vip(int drawn_fb) { (void)drawn_fb; }

/* Sound backend. MUST report success: sound_init() frees the wave buffers if
 * the backend fails and sound_update() then writes freed memory (proven UAF on
 * the harness). sound_push_backend() (in vb_audio.c) accumulates completed VSU
 * buffers; vb_audio_drain() downmixes them to the retro-go mono mixer per frame. */
static int16_t *s_wavebufs[BUF_COUNT];
bool sound_init_backend(int16_t **bufs) {
    for (int i = 0; i < BUF_COUNT; i++) s_wavebufs[i] = bufs[i];
    return true;
}
void sound_close_backend(void)  {}
void sound_pause_backend(void)  {}
void sound_resume_backend(void) {}

/* VB VSU -> retro-go mixer bridge (vb_audio.c). */
void vb_audio_drain(int16_t *out, int len, int32_t factor);
void vb_audio_reset(void);

extern unsigned int vb_rom_mask;

/* ---- save / load (verified vb_savestate.c module) ------------------------ */
int  vb_state_save(const char *path);
int  vb_state_load(const char *path);

static bool LoadState(const char *savePathName) { return vb_state_load(savePathName) == 0; }
static bool SaveState(const char *savePathName) { return vb_state_save(savePathName) == 0; }
static void *Screenshot(void) { return NULL; }

/* ---- ROM (flash XIP) ----------------------------------------------------- */

static size_t vb_getromdata(unsigned char **data)
{
    /* Emulator heap starts just past this overlay's BSS. */
    ram_start = (uint32_t)&_OVERLAY_VB_BSS_END;

    uint32_t src_size = 0;
    const unsigned char *src =
        odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &src_size, false);
    *data = (unsigned char *)src;
    return src_size;
}

/* Cheap, ROM-stable stamp so savestates are matched to their ROM (a full CRC32
 * isn't needed — only self-consistency across save/reboot/load). */
static uint32_t vb_rom_stamp(const unsigned char *rom, uint32_t len)
{
    uint32_t h = 2166136261u ^ len;
    uint32_t step = len > 4096 ? len / 4096 : 1;
    for (uint32_t i = 0; i < len; i += step) { h ^= rom[i]; h *= 16777619u; }
    return h;
}

/* ---- input --------------------------------------------------------------- */

static void vb_input_read(odroid_gamepad_state_t *j)
{
    uint32_t k = 0;
    /* Physical D-pad -> VB left D-pad; A/B and Start/Select map 1:1.
     * VB right-D-pad + L/R need an overlay/mode-toggle (follow-up). */
    if (j->values[ODROID_INPUT_UP])     k |= VB_LPAD_U;
    if (j->values[ODROID_INPUT_DOWN])   k |= VB_LPAD_D;
    if (j->values[ODROID_INPUT_LEFT])   k |= VB_LPAD_L;
    if (j->values[ODROID_INPUT_RIGHT])  k |= VB_LPAD_R;
    if (j->values[ODROID_INPUT_A])      k |= VB_KEY_A;
    if (j->values[ODROID_INPUT_B])      k |= VB_KEY_B;
    if (j->values[ODROID_INPUT_START])  k |= VB_KEY_START;
    if (j->values[ODROID_INPUT_SELECT]) k |= VB_KEY_SELECT;

    vb_state->tHReg.SLB = (BYTE)(k & 0xFF);
    vb_state->tHReg.SHB = (BYTE)((k >> 8) & 0xFF);
}

/* ---- video: VB 384x224 2-bit red -> 320x240 RGB565 (single eye) ---------- */

static void vb_blit(void)
{
    int dfb = vb_state->tVIPREG.tDisplayedFB;

    /* Composite the just-drawn frame into DISPLAY_RAM via the software renderer
     * (same sequence as red-viper's linux soft path). */
    if (vb_state->tVIPREG.tFrame == 0 && !vb_state->tVIPREG.drawing &&
        (vb_state->tVIPREG.XPCTRL & XPEN)) {
        if (tDSPCACHE.CharCacheInvalid) update_texture_cache_soft();
        video_soft_render(!dfb);
        tDSPCACHE.CharCacheInvalid = false;
        /* BGCacheInvalid only exists under NEED_BG_CACHE (disabled in this build). */
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }

    /* Left-eye framebuffer: 2 bits/pixel, column-major (8 px per 16-bit word). */
    const uint16_t *vb_fb =
        (const uint16_t *)(vb_state->V810_DISPLAY_RAM.pmemory + 0x8000 * dfb);

    int bri[4];
    bri[0] = 0;
    bri[1] = vb_state->tVIPREG.BRTA;
    bri[2] = vb_state->tVIPREG.BRTB;
    bri[3] = vb_state->tVIPREG.BRTA + vb_state->tVIPREG.BRTB + vb_state->tVIPREG.BRTC;

    uint16_t *out = (uint16_t *)lcd_get_active_buffer();

    /* Aspect-correct auto-fit (device convention, cf. Lynx center/letterbox):
     * preserve the VB 384:224 ratio — full 320 width, scaled to 186 rows and
     * vertically centered, with black letterbox bars top/bottom. */
    const int dst_w = GW_LCD_WIDTH;                 /* 320 (full width) */
    const int dst_h = GW_LCD_WIDTH * 224 / 384;     /* 186 (keeps 384:224) */
    const int y0    = (GW_LCD_HEIGHT - dst_h) / 2;  /* 27 */

    memset(out, 0, (size_t)GW_LCD_WIDTH * GW_LCD_HEIGHT * sizeof(uint16_t));

    for (int ry = 0; ry < dst_h; ry++) {
        int sy = ry * 224 / dst_h;                  /* nearest-neighbour scale */
        const uint16_t *col = vb_fb + (sy >> 3);
        int shift = (sy & 7) * 2;
        uint16_t *dst = out + (y0 + ry) * GW_LCD_WIDTH;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = dx * 384 / dst_w;
            int v = (col[sx * 32] >> shift) & 3;
            int b = bri[v] * 2;
            if (b > 255) b = 255;
            dst[dx] = (uint16_t)((b >> 3) << 11);   /* red channel only */
        }
    }
}

/* ---- entry --------------------------------------------------------------- */

int app_main_vb(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    odroid_system_init(APPID_VB, SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, &Screenshot, NULL, NULL, NULL);

    /* getromdata() sets ram_start (heap past overlay BSS) — MUST precede the
     * v810_init() region allocations, which pull from that heap. */
    unsigned char *rom_ptr = NULL;
    size_t rom_len = vb_getromdata(&rom_ptr);

    setDefaults();
    is_multiplayer = false;
    v810_init();
    replay_init();

    /* Point the CPU at the flash-resident ROM (XIP) with a power-of-2 mirror. */
    V810_ROM1.pmemory  = rom_ptr;
    V810_ROM1.lowaddr  = 0x07000000;
    V810_ROM1.size     = rom_len;
    V810_ROM1.highaddr = 0x07000000 + rom_len - 1;
    V810_ROM1.off      = (size_t)rom_ptr - 0x07000000;
    vb_rom_mask        = (unsigned int)(rom_len - 1);
    tVBOpt.CRC32       = vb_rom_stamp(rom_ptr, (uint32_t)rom_len);

    v810_reset();
    clearCache();
    video_soft_init();
    sound_init();

    audio_start_playing(SAMPLE_RATE / 50);

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
    } else {
        lcd_clear_buffers();
    }

    odroid_gamepad_state_t joystick = {0};
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };

    while (true) {
        wdog_refresh();
        bool drawFrame = common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, options, &vb_blit);
        vb_input_read(&joystick);

        v810_run();

        if (drawFrame) {
            vb_blit();
            lcd_swap();
        }

        /* Downmix the VB VSU frames to the retro-go mono mixer (Lynx technique). */
        if (common_emu_sound_loop_is_muted()) {
            vb_audio_reset();
        } else {
            vb_audio_drain(audio_get_active_buffer(), audio_get_buffer_length(),
                           common_emu_sound_get_volume());
        }
        common_emu_sound_sync(false);
    }

    return 0;
}
