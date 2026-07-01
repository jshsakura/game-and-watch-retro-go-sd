/* ZX Spectrum 48K porting layer (floooh/chips zx.h core).
 * Video+input first; audio is a no-op callback for v1 (added later, like PCE-CD
 * shipping before its CD audio). Mirrors the videopac/pce porting pattern. */
#include <odroid_system.h>
#include <string.h>

#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include "rom_manager.h"
#include "main_zxs.h"

#include "chips/chips_common.h"
#include "chips/z80.h"
#include "chips/beeper.h"
#include "chips/ay38910.h"
#include "chips/kbd.h"
#include "chips/mem.h"
#include "chips/clk.h"
#include "chips/zx.h"

#define ZX_AUDIO_SAMPLE_RATE 22050
#define ZX_FPS               50            /* PAL */
#define ZX_AUDIO_SAMPLES     (ZX_AUDIO_SAMPLE_RATE / ZX_FPS)   /* 441 / frame */
#define RGB565(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
/* Auto-fit: the chips ZX display is 320x256 with a 32px border on every side,
 * so the real Spectrum screen is the 256x192 content at (32,32). Scale just that
 * content to fill the 320x240 LCD — both are 4:3, so it's an exact 1.25x with no
 * distortion and no wasted border. */
#define ZX_CONTENT_LEFT 32
#define ZX_CONTENT_TOP  32
#define ZX_CONTENT_W    256
#define ZX_CONTENT_H    192

static zx_t      zx;
static uint16_t  zx_pal565[16];
static bool      zx_is128;

/* ---- audio: the chips zx core emits float samples ([-1,1]) via this callback
 * during zx_exec (in batches of desc.audio.num_samples). Accumulate them as int16,
 * then zx_pcm_submit() hands one frame's worth to the device DMA buffer. Carrying
 * the remainder between frames keeps 441-per-frame vs the 128-batch callback from
 * clicking. ---- */
static int16_t  zx_snd[ZX_AUDIO_SAMPLES * 2 + 128];
static int      zx_snd_w;

static void audio_cb(const float *s, int n, void *u)
{
    (void)u;
    const int cap = (int)(sizeof(zx_snd) / sizeof(zx_snd[0]));
    for (int i = 0; i < n && zx_snd_w < cap; i++) {
        float v = s[i];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        zx_snd[zx_snd_w++] = (int16_t)(v * 22000.0f);
    }
}

static void zx_pcm_submit(void)
{
    int16_t *out = audio_get_active_buffer();
    int      len = audio_get_buffer_length();
    int      mute = common_emu_sound_loop_is_muted();
    int32_t  factor = common_emu_sound_get_volume();
    for (int i = 0; i < len; i++) {
        int32_t s = (i < zx_snd_w) ? zx_snd[i] : 0;
        if (mute) { s = 0; }
        else {
            s = (s * factor) >> 8;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        }
        out[i] = (int16_t)s;
    }
    int rem = zx_snd_w - len;           /* carry any over-produced samples forward */
    if (rem > 0) { memmove(zx_snd, zx_snd + len, (size_t)rem * sizeof(int16_t)); zx_snd_w = rem; }
    else         { zx_snd_w = 0; }
}

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
    /* Draw to the ACTIVE buffer — same as a7800/lynx — because the PAUSE popup
     * (odroid_overlay) composites onto lcd_get_active_buffer(). Drawing to the
     * inactive buffer here made the menu render over a stale/other frame. */
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    for (int dy = 0; dy < GW_LCD_HEIGHT; dy++) {
        int sy = ZX_CONTENT_TOP + (dy * ZX_CONTENT_H / GW_LCD_HEIGHT);
        const uint8_t *src = &zx.fb[sy * ZX_FRAMEBUFFER_WIDTH + ZX_CONTENT_LEFT];
        uint16_t *dst = &out[dy * GW_LCD_WIDTH];
        for (int dx = 0; dx < GW_LCD_WIDTH; dx++) {
            int sx = dx * ZX_CONTENT_W / GW_LCD_WIDTH;   /* 256 -> 320 (1.25x) */
            dst[dx] = zx_pal565[src[sx] & 15];
        }
    }
    /* In-game overlay (battery, etc.) on top, like the other cores. */
    common_ingame_overlay();
}

static bool load_bios(zx_desc_t *desc)
{
    uint32_t sz = 0;
    /* 48K only for v1: read the 16K ROM straight off the SD (firmware /bios/<sys>/
     * convention; cached in flash, zx_init memcpy's it into core RAM). */
    const uint8_t *p = (const uint8_t *)odroid_overlay_cache_file_in_flash("/bios/zxs/48.rom", &sz, false);
    if (!p || sz < 0x4000) {
        printf("[ZX] missing /bios/zxs/48.rom (got %u)\n", (unsigned)sz);
        return false;
    }
    desc->roms.zx48k.ptr  = (void *)p;
    desc->roms.zx48k.size = 0x4000;
    return true;
}

static bool init(void)
{
    odroid_system_init(APPID_GB, ZX_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&LoadState, &SaveState, NULL, NULL, NULL, NULL);
    /* Start the audio DMA: common_emu_sound_sync busy-waits on the DMA counter every
     * frame, so without this the FIRST frame hangs forever (no DMA tick). */
    audio_start_playing(ZX_AUDIO_SAMPLES);

    zx_desc_t desc = {0};
    desc.type           = ZX_TYPE_48K;
    desc.joystick_type  = ZX_JOYSTICKTYPE_KEMPSTON;
    desc.audio.callback.func = audio_cb;
    desc.audio.num_samples   = 128;
    desc.audio.sample_rate   = ZX_AUDIO_SAMPLE_RATE;
    desc.audio.beeper_volume = 0.5f;
    desc.audio.ay_volume     = 0.5f;
    zx_is128 = false;

    if (!load_bios(&desc)) return false;   /* missing /bios/zxs/48.rom -> bounce to menu, not HardFault */
    zx_init(&zx, &desc);
    zx_build_palette();

    /* Load the .z80 game (cached to flash, parsed read-only by zx_quickload). */
    uint32_t gsz = 0;
    const uint8_t *g = (const uint8_t *)odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &gsz, false);
    if (g && gsz) {
        bool ok = zx_quickload(&zx, (chips_range_t){ .ptr = (void *)g, .size = gsz });
        printf("[ZX] quickload %s -> %d (%u bytes)\n", ACTIVE_FILE->path, ok, (unsigned)gsz);
    }
    return true;
}

/* ---- configurable button->key (GAME / TIME / B), picked live from the PAUSE
 * menu (D-pad L/R cycles) — the same pattern as the C64/Amstrad cores. Most ZX
 * games need a keyboard key to start / pick controls (e.g. "0=start, L=load"),
 * which the Kempston pad can't send. Uses kbd_key_down() DIRECTLY so Space and
 * the arrows aren't stolen by zx_key_down()'s Kempston remap. ---- */
struct zx_key { const char *name; int code; };
static const struct zx_key zx_keys[] = {
    {"Space",0x20},{"Enter",0x0D},
    {"0",'0'},{"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},{"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},
    {"Q",'q'},{"W",'w'},{"E",'e'},{"R",'r'},{"T",'t'},{"Y",'y'},{"U",'u'},{"I",'i'},{"O",'o'},{"P",'p'},
    {"A",'a'},{"S",'s'},{"D",'d'},{"F",'f'},{"G",'g'},{"H",'h'},{"J",'j'},{"K",'k'},{"L",'l'},
    {"Z",'z'},{"X",'x'},{"C",'c'},{"V",'v'},{"B",'b'},{"N",'n'},{"M",'m'},
};
#define ZX_NKEYS ((int)(sizeof(zx_keys) / sizeof(zx_keys[0])))
static int zx_key_game = 1;   /* GAME -> Enter (default) */
static int zx_key_time = 0;   /* TIME -> Space (default) */
static int zx_key_b    = 2;   /* B    -> 0     (default; e.g. "0=start") */

static bool zx_keycfg_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r, int *idx) {
    (void)r;
    if (e == ODROID_DIALOG_PREV) *idx = (*idx + ZX_NKEYS - 1) % ZX_NKEYS;
    if (e == ODROID_DIALOG_NEXT) *idx = (*idx + 1) % ZX_NKEYS;
    strcpy(o->value, zx_keys[*idx].name);
    return e == ODROID_DIALOG_ENTER;
}
static bool zx_game_key_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return zx_keycfg_cb(o, e, r, &zx_key_game); }
static bool zx_time_key_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return zx_keycfg_cb(o, e, r, &zx_key_time); }
static bool zx_b_key_cb   (odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return zx_keycfg_cb(o, e, r, &zx_key_b); }

static void zx_btn_key(bool pressed, int keyidx) {
    if (pressed) kbd_key_down(&zx.kbd, zx_keys[keyidx].code);
    else         kbd_key_up(&zx.kbd, zx_keys[keyidx].code);
}

/* ---- on-device input diagnostics: append one line per button edge to the shared
 * /device_diag.txt (sd_save_log, shared by Lynx save + ZX input — delete it before a
 * clean test). Shows whether a button is even detected and which ZX key it sends. ---- */
extern void sd_save_log(const char *line);
extern void sd_save_log_boot(const char *line);

static void zx_log_input(odroid_gamepad_state_t *j) {
    static uint8_t pg, pt, pb, pa, pdp;
    char s[96];
    uint8_t g = j->values[ODROID_INPUT_START], t = j->values[ODROID_INPUT_SELECT];
    uint8_t b = j->values[ODROID_INPUT_B],     a = j->values[ODROID_INPUT_A];
    uint8_t dp = j->values[ODROID_INPUT_UP] | j->values[ODROID_INPUT_DOWN] |
                 j->values[ODROID_INPUT_LEFT] | j->values[ODROID_INPUT_RIGHT];
    if (g != pg)  { snprintf(s, sizeof s, "[zxs] GAME %s -> key '%s'", g ? "DN" : "up", zx_keys[zx_key_game].name); sd_save_log(s); pg = g; }
    if (t != pt)  { snprintf(s, sizeof s, "[zxs] TIME %s -> key '%s'", t ? "DN" : "up", zx_keys[zx_key_time].name); sd_save_log(s); pt = t; }
    if (b != pb)  { snprintf(s, sizeof s, "[zxs] B %s -> key '%s'",    b ? "DN" : "up", zx_keys[zx_key_b].name);    sd_save_log(s); pb = b; }
    if (a != pa)  { snprintf(s, sizeof s, "[zxs] A(fire) %s", a ? "DN" : "up"); sd_save_log(s); pa = a; }
    if (dp != pdp){ snprintf(s, sizeof s, "[zxs] DPAD %s", dp ? "active" : "idle"); sd_save_log(s); pdp = dp; }
}

void app_main_zx(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;
    odroid_gamepad_state_t joystick;

    if (!init()) return;   /* BIOS missing -> return to launcher instead of running garbage */

    { char s[96]; snprintf(s, sizeof s, "[zxs] input diag build %s %s  GAME=%s TIME=%s B=%s",
        __DATE__, __TIME__, zx_keys[zx_key_game].name, zx_keys[zx_key_time].name, zx_keys[zx_key_b].name);
      sd_save_log_boot(s); }

    if (load_state)
        odroid_system_emu_load_state(save_slot);

    while (true) {
        wdog_refresh();
        common_emu_frame_loop();

        odroid_input_read_gamepad(&joystick);
        zx_log_input(&joystick);   /* SD-log every button edge + the key it sends */
        /* PAUSE menu entries: pick which ZX key GAME / TIME / B send (D-pad L/R
         * cycles the value). Rebuilt each frame so the shown name stays current. */
        char game_kn[8], time_kn[8], b_kn[8];
        strcpy(game_kn, zx_keys[zx_key_game].name);
        strcpy(time_kn, zx_keys[zx_key_time].name);
        strcpy(b_kn,    zx_keys[zx_key_b].name);
        odroid_dialog_choice_t options[] = {
            { 100, "GAME key", game_kn, 1, &zx_game_key_cb },
            { 101, "TIME key", time_kn, 1, &zx_time_key_cb },
            { 102, "B key",    b_kn,    1, &zx_b_key_cb },
            ODROID_DIALOG_CHOICE_LAST,
        };
        /* Non-NULL repaint: the PAUSE menu calls this to redraw the frame behind
         * the overlay — passing NULL jumps to PC=0 (HardFault) when PAUSE is
         * pressed (cf. custom-loop-core-integration). */
        common_emu_input_loop(&joystick, options, &zx_blit);
        common_emu_input_loop_handle_turbo(&joystick);

        uint8_t m = 0;
        if (joystick.values[ODROID_INPUT_LEFT])  m |= ZX_JOYSTICK_LEFT;
        if (joystick.values[ODROID_INPUT_RIGHT]) m |= ZX_JOYSTICK_RIGHT;
        if (joystick.values[ODROID_INPUT_UP])    m |= ZX_JOYSTICK_UP;
        if (joystick.values[ODROID_INPUT_DOWN])  m |= ZX_JOYSTICK_DOWN;
        if (joystick.values[ODROID_INPUT_A])     m |= ZX_JOYSTICK_BTN;
        zx_joystick(&zx, m);
        /* GAME / TIME / B -> the configurable keyboard keys picked in the PAUSE
         * menu (kbd_key_down goes straight to the matrix, bypassing the Kempston
         * remap that would steal Space / arrows). */
        zx_btn_key(joystick.values[ODROID_INPUT_START],  zx_key_game);
        zx_btn_key(joystick.values[ODROID_INPUT_SELECT], zx_key_time);
        zx_btn_key(joystick.values[ODROID_INPUT_B],      zx_key_b);

        zx_exec(&zx, 19968);     /* one 50Hz PAL frame (fills zx_snd via audio_cb) */
        zx_blit();
        lcd_swap();

        zx_pcm_submit();         /* hand this frame's samples to the DMA buffer */
        common_emu_sound_sync(false);
    }
}
