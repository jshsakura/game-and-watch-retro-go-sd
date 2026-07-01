/* Commodore 64 device porting layer — Frodo core (cebix/frodo-go), .d64 via the
 * virtual 1541. Replaces the ESP Display/DigitalRenderer/main with G&W glue.
 * Mirrors the Lynx C++-overlay pattern. ROMs from /bios/c64/, disk from SD path. */
extern "C" {
#include <odroid_system.h>
#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include "rom_manager.h"
#include "main_c64.h"
#include "gw_linker.h"
#include "cpp_init_array.h"
#include "gw_audio.h"
void  heap_itc_alloc(bool itc);
}
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "C64.h"
#include "Display.h"
#include "Prefs.h"
#include "DigitalRenderer.h"

#define RGB565(r,g,b) ((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
/* G&W RAM-fit: Frodo bitmap is now DISPLAY_X(340) x DISPLAY_Y(208). The 40-column
 * display window occupies bytes 20..339 of each line, so crop the 320px content at
 * x=20. The 208 rows are letterboxed (centred) into the 240-row LCD. */
#define C64_CROP_X 20
#define C64_LETTERBOX_Y ((HEIGHT - DISPLAY_Y) / 2)   /* (240-208)/2 = 16 */

/* IsFrodoSC is defined in C64.cpp (one definition kept there). */

static const uint8 c64rgb[16][3] = {
    {0x00,0x00,0x00},{0xff,0xff,0xff},{0x68,0x37,0x2b},{0x70,0xa4,0xb2},
    {0x6f,0x3d,0x86},{0x58,0x8d,0x43},{0x35,0x28,0x79},{0xb8,0xc7,0x6f},
    {0x6f,0x4f,0x25},{0x43,0x39,0x00},{0x9a,0x67,0x59},{0x44,0x44,0x44},
    {0x6c,0x6c,0x6c},{0x9a,0xd2,0x84},{0x6c,0x5e,0xb5},{0x95,0x95,0x95},
};
static uint16_t s_pal565[256];
static uint8    s_bitmap[DISPLAY_X * DISPLAY_Y];
static uint32_t s_frame = 0;

/* ---- C64Display (device) ---- */
C64Display::C64Display(C64 *the_c64) : TheC64(the_c64)
{
    quit_requested = false;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    for (int i = 0; i < 16; i++) s_pal565[i] = RGB565(c64rgb[i][0], c64rgb[i][1], c64rgb[i][2]);
}
C64Display::~C64Display() {}
uint8 *C64Display::BitmapBase(void) { return s_bitmap; }
int    C64Display::BitmapXMod(void) { return DISPLAY_X; }
void   C64Display::UpdateLEDs(int,int,int,int) {}
void   C64Display::Speedometer(int) {}
bool   C64Display::NumLock(void) { return false; }
void   C64Display::NewPrefs(Prefs *) {}
void   C64Display::InitColors(uint8 *colors) { for (int i = 0; i < 256; i++) colors[i] = i & 0x0f; }

/* autostart: feed LOAD"*",8,1 / RUN into the C64 keyboard buffer ($0277, count $C6) */
static const char *s_type = NULL;
static int s_typepos = 0;
static bool s_is_prg = false;   /* raw .prg (RAM-injected) vs .d64 (virtual 1541) */

/* Deferred save/load. The pause-menu Save/Load handlers run in FIRMWARE context where the
 * overlay's live C64 object can read back as garbage (the Lynx veneer/clobber hazard), so
 * they only RECORD the op + path; the real SaveSnapshot/LoadSnapshot runs in
 * C64Display::Update where TheC64 is valid. Snapshot = VIC+SID+CIA+CPU + full 64K RAM +
 * colour RAM (C64.cpp SaveCPUState); the flash-resident ROM pointers aren't in it and stay
 * valid across load. */
static char s_state_path[300];
static int  s_state_op;   /* 0 none, 1 save, 2 load */
static bool c64_SaveState(const char *path)
{ strncpy(s_state_path, path, sizeof(s_state_path) - 1); s_state_path[sizeof(s_state_path) - 1] = 0; s_state_op = 1; return true; }
static bool c64_LoadState(const char *path)
{ strncpy(s_state_path, path, sizeof(s_state_path) - 1); s_state_path[sizeof(s_state_path) - 1] = 0; s_state_op = 2; return true; }

/* ---- configurable button->C64-key map (Amstrad pattern) -------------------
 * The G&W has too few buttons for a full C64 keyboard, so let the user choose which
 * C64 key the GAME / TIME / B buttons send, live from the pause menu (dpad L/R cycles).
 * A = joystick fire is fixed. row/col = C64 keyboard matrix; row 0xff = joystick fire. */
struct c64_key { const char *name; uint8 row, col; };
static const struct c64_key c64_keys[] = {
    {"Space",   7, 4}, {"Return",  0, 1}, {"F1",      0, 4}, {"F3",      0, 5},
    {"F5",      0, 6}, {"F7",      0, 3}, {"Y",       3, 1}, {"N",       4, 7},
    {"Run/Stop",7, 7}, {"1",       7, 0}, {"2",       7, 3}, {"3",       1, 0},
    {"Fire",    0xff, 0xff},
};
#define C64_NKEYS ((int)(sizeof(c64_keys) / sizeof(c64_keys[0])))
static int c64_key_game = 0;   /* GAME  -> Space  (default) */
static int c64_key_time = 1;   /* TIME  -> Return (default) */
static int c64_key_b    = 7;   /* B     -> N      (default; the natural fastload answer) */

static bool c64_keycfg_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat, int *idx)
{
    (void)repeat;
    if (event == ODROID_DIALOG_PREV) *idx = (*idx + C64_NKEYS - 1) % C64_NKEYS;
    if (event == ODROID_DIALOG_NEXT) *idx = (*idx + 1) % C64_NKEYS;
    strcpy(option->value, c64_keys[*idx].name);
    return event == ODROID_DIALOG_ENTER;
}
static bool c64_game_key_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return c64_keycfg_cb(o, e, r, &c64_key_game); }
static bool c64_time_key_cb(odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return c64_keycfg_cb(o, e, r, &c64_key_time); }
static bool c64_b_key_cb   (odroid_dialog_choice_t *o, odroid_dialog_event_t e, uint32_t r) { return c64_keycfg_cb(o, e, r, &c64_key_b); }

/* Apply a configured key to the C64 matrix (or the joystick fire mask for "Fire"). */
static inline void c64_apply_key(int idx, uint8 *key_matrix, uint8 *rev_matrix, uint8 *fire_mask)
{
    uint8 row = c64_keys[idx].row, col = c64_keys[idx].col;
    if (row == 0xff) { if (fire_mask) *fire_mask &= ~0x10; return; }   /* joystick fire */
    if (key_matrix && rev_matrix) { key_matrix[row] &= ~(1 << col); rev_matrix[col] &= ~(1 << row); }
}

/* A raw .prg file isn't a disk image, so the virtual 1541 can't mount it. Load it
 * straight into C64 RAM at its 2-byte header address, then RUN. Done after the
 * KERNAL has cleared RAM (~frame 120) so the program survives reset. */
static void inject_prg(C64 *c64)
{
    FILE *f = fopen(ACTIVE_FILE->path, "rb");
    if (!f) return;
    uint8 hdr[2];
    if (fread(hdr, 1, 2, f) == 2) {
        uint16_t addr = (uint16_t)(hdr[0] | (hdr[1] << 8));
        int n = (int)fread(&c64->RAM[addr], 1, 0x10000 - addr, f);
        uint16_t end = (uint16_t)(addr + n);
        /* Point BASIC's end-of-program vectors past the loaded code so RUN works
         * (PRGs at $0801 are BASIC-launchable; most game PRGs have a SYS stub). */
        c64->RAM[0x2d] = end & 0xff; c64->RAM[0x2e] = end >> 8;   /* VARTAB */
        c64->RAM[0x2f] = end & 0xff; c64->RAM[0x30] = end >> 8;   /* ARYTAB */
        c64->RAM[0x31] = end & 0xff; c64->RAM[0x32] = end >> 8;   /* STREND */
    }
    fclose(f);
}

/* Scale the WHOLE 340x208 Frodo picture to fit the 320-wide LCD (keep aspect,
 * letterbox top/bottom). Fixes the old 320-wide crop that cut the right edge and
 * left the image shifted. Writes the ACTIVE buffer and does NOT swap — the caller
 * (or the pause-menu overlay, which passes this as its repaint) owns the swap. That
 * matches the Lynx/gb_tgbdual pattern; the previous inactive-buffer + swap here made
 * the overlay double-swap and flicker. */
static void c64_repaint(void)
{
    /* The C64 40-column picture is a FIXED 320px — same as the LCD width — so blit it 1:1,
     * no scaling. Take the centred 320px window of the 340-wide Frodo bitmap and letterbox
     * the 208 rows into the 240-tall LCD. (X0 is the only knob if the window is a few px
     * off; centring the 20px total border is the natural fit.) */
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    const int X0 = 0x20;                                 /* VIC COL40_XSTART: content left edge */
    int cw = DISPLAY_X - X0;                             /* visible content cols (340-32=308) */
    if (cw > WIDTH) cw = WIDTH;
    const int Y0 = (HEIGHT - DISPLAY_Y) / 2;             /* (240-208)/2 = 16 */
    /* No per-frame whole-buffer memset — that raced the LCD scan-out (a black bar crept up
     * the screen). The borders are cleared ONCE at launch (lcd_clear_buffers); each frame
     * we only overwrite the fixed content rectangle + its right strip, so the borders stay
     * black on both buffers. */
    for (int y = 0; y < DISPLAY_Y; y++) {
        const uint8 *src = &s_bitmap[y * DISPLAY_X + X0];
        uint16_t *dst = &out[(Y0 + y) * WIDTH];
        for (int x = 0; x < cw; x++)
            dst[x] = s_pal565[src[x] & 0x0f];
        for (int x = cw; x < WIDTH; x++) dst[x] = 0;    /* right border strip */
    }
}


static void c64_audio_drain(void);

void C64Display::Update(void)
{
    s_frame++;
    wdog_refresh();

    /* Run any pending menu save/load HERE, where TheC64 is the valid live pointer. */
    if (s_state_op) {
        if (s_state_op == 1) TheC64->SaveSnapshot(s_state_path);
        else                 TheC64->LoadSnapshot(s_state_path);
        s_state_op = 0;
    }

    /* autostart sequencing */
    if (s_is_prg) {
        if      (s_frame == 120) { inject_prg(TheC64); }
        else if (s_frame == 150) { s_type = "RUN\r"; s_typepos = 0; }
    } else {
        if      (s_frame == 150) { s_type = "LOAD\"*\",8,1\r"; s_typepos = 0; }
        else if (s_frame == 420) { s_type = "RUN\r";          s_typepos = 0; }
    }
    if (s_type && TheC64->RAM[0xC6] == 0) {
        int n = 0;
        while (s_type[s_typepos] && n < 10) { TheC64->RAM[0x0277 + n] = (uint8)s_type[s_typepos++]; n++; }
        TheC64->RAM[0xC6] = (uint8)n;
        if (!s_type[s_typepos]) { s_type = NULL; s_typepos = 0; }
    }

    /* Auto-warp during disk loading: the standard KERNAL LOAD is slow in real time
     * (~30-50s on a 50fps-paced device → looks frozen at "LOADING"). While the virtual
     * 1541 is delivering sectors, run flat-out — blit only 1 frame in 16 and SKIP the
     * audio/frame sync so Frodo emulates the next frame immediately. Stops ~0.5s after
     * the last disk read, then normal paced play resumes. (Mirrors VICE autoloadwarp.) */
    extern volatile unsigned int g_c64_disk_reads;
    static unsigned int s_last_reads = 0;
    static int s_warp_idle = 999;
    if (g_c64_disk_reads != s_last_reads) { s_last_reads = g_c64_disk_reads; s_warp_idle = 0; }
    else if (s_warp_idle < 999) s_warp_idle++;
    const bool warp = (s_warp_idle < 25);
    if (warp && (s_frame & 0x0F) != 0)
        return;                       /* skip blit + sync -> full-speed load */

    /* INPUT FIRST, THEN DRAW. common_emu_input_loop (VOLUME/SET + dir) sets the volume/
     * brightness overlay flag; it must run BEFORE common_ingame_overlay() draws the bar,
     * or the bar is never shown (the previous order drew, THEN armed -> nothing appeared,
     * "volume doesn't work"). It also opens the pause/quit menu (repaint = c64_repaint). */
    odroid_gamepad_state_t js;
    odroid_input_read_gamepad(&js);
    char game_kn[12], time_kn[12], b_kn[12];
    strcpy(game_kn, c64_keys[c64_key_game].name);
    strcpy(time_kn, c64_keys[c64_key_time].name);
    strcpy(b_kn,    c64_keys[c64_key_b].name);
    odroid_dialog_choice_t options[] = {
        { 100, "GAME key", game_kn, 1, &c64_game_key_cb },
        { 101, "TIME key", time_kn, 1, &c64_time_key_cb },
        { 102, "B key",    b_kn,    1, &c64_b_key_cb },
        ODROID_DIALOG_CHOICE_LAST
    };
    common_emu_frame_loop();
    common_emu_input_loop(&js, options, c64_repaint);
    common_emu_input_loop_handle_turbo(&js);

    c64_repaint();
    common_ingame_overlay();   /* draws the volume/brightness/speedup bar armed just above */
    lcd_swap();

    c64_audio_drain();         /* pull one frame of SID samples into the DMA buffer */
    if (!warp)
        common_emu_sound_sync(false);   /* during warp, don't pace to audio */
}

#ifdef __riscos__
void C64Display::PollKeyboard(uint8*,uint8*,uint8*,uint8*) {}
#else
void C64Display::PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix, uint8 *joystick)
{
    odroid_gamepad_state_t js;
    odroid_input_read_gamepad(&js);
    uint8 m = 0xff;  /* active-low */
    if (js.values[ODROID_INPUT_UP])    m &= ~0x01;
    if (js.values[ODROID_INPUT_DOWN])  m &= ~0x02;
    if (js.values[ODROID_INPUT_LEFT])  m &= ~0x04;
    if (js.values[ODROID_INPUT_RIGHT]) m &= ~0x08;
    if (js.values[ODROID_INPUT_A])     m &= ~0x10;  /* A = joystick fire (fixed) */

    /* GAME / TIME / B send the C64 key the user picked in the pause menu (default
     * Space / Return / N). The CIA resets the matrix to 0xff once at reset, so re-set
     * it every poll or a key sticks. A key mapped to "Fire" pulls the joystick fire. */
    if (key_matrix && rev_matrix)
        for (int i = 0; i < 8; i++) { key_matrix[i] = 0xff; rev_matrix[i] = 0xff; }
    if (js.values[ODROID_INPUT_START])  c64_apply_key(c64_key_game, key_matrix, rev_matrix, &m);
    if (js.values[ODROID_INPUT_SELECT]) c64_apply_key(c64_key_time, key_matrix, rev_matrix, &m);
    if (js.values[ODROID_INPUT_B])      c64_apply_key(c64_key_b,    key_matrix, rev_matrix, &m);

    if (joystick) *joystick = m;
}
#endif

long ShowRequester(char *str, char *b1, char *b2) { (void)b1; (void)b2; printf("[c64] %s\n", str?str:""); return 1; }

/* ---- SID audio bridge (SID re-enabled; TriTable now heap-alloc'd so it fits) ----
 * The real Frodo DigitalRenderer generates the SID DSP and PUSHES stereo via
 * c64sid_audio_submit() from SID::EmulateLine (per raster line). We ring-buffer the
 * mono mix; C64Display::Update() drains one device-DMA frame (441 @ 22050/50) into
 * audio_get_active_buffer before common_emu_sound_sync (same output path as Lynx). */
#define C64_SND_RING 2048
static int16_t s_snd_ring[C64_SND_RING];
static volatile int s_snd_wr = 0, s_snd_rd = 0;

extern "C" void c64sid_audio_init(int rate) { (void)rate; }
extern "C" void c64sid_audio_terminate(void) {}
extern "C" void c64sid_audio_submit(const int16_t *stereo, int frames)
{
    for (int i = 0; i < frames; i++) {
        int nx = (s_snd_wr + 1) & (C64_SND_RING - 1);
        if (nx == s_snd_rd) break;            /* ring full: drop */
        s_snd_ring[s_snd_wr] = stereo[i * 2]; /* left -> mono */
        s_snd_wr = nx;
    }
}

static void c64_audio_drain(void)
{
    int16_t *out = audio_get_active_buffer();
    int      len = audio_get_buffer_length();
    int      mute = common_emu_sound_loop_is_muted();
    int32_t  factor = common_emu_sound_get_volume();
    for (int i = 0; i < len; i++) {
        int16_t s = 0;
        if (s_snd_rd != s_snd_wr) { s = s_snd_ring[s_snd_rd]; s_snd_rd = (s_snd_rd + 1) & (C64_SND_RING - 1); }
        int32_t v = mute ? 0 : (((int32_t)s * factor) >> 8);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}

/* Map a ROM file into flash and return a read-only pointer (>= want bytes), or NULL. */
static const uint8_t *flash_rom(const char *path, uint32_t want)
{
    uint32_t sz = 0;
    const uint8_t *p = (const uint8_t *)odroid_overlay_cache_file_in_flash(path, &sz, false);
    if (!p || sz < want) { printf("[c64] missing %s (%u/%u)\n", path, (unsigned)sz, (unsigned)want); return NULL; }
    return p;
}

/* Copy a ROM file from flash into a writable buffer (used for the patched Kernal). */
static bool load_rom(const char *path, uint8 *dst, uint32_t want)
{
    const uint8_t *p = flash_rom(path, want);
    if (!p) return false;
    memcpy(dst, p, want);
    return true;
}

/* Read a ROM straight from SD into RAM (NO flash cache). The cache can return clobbered
 * bytes after wrapping; the BIOS is tiny so a direct fread is both safe and simple. */
static bool read_bios_file(const char *path, uint8 *dst, uint32_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("[c64] missing %s\n", path); return false; }
    size_t n = fread(dst, 1, want, f);
    fclose(f);
    if (n != want) { printf("[c64] %s short read %u/%u\n", path, (unsigned)n, (unsigned)want); return false; }
    return true;
}

extern "C" void app_main_c64(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)start_paused;

    /* Run the Frodo C++ static constructors (global ThePrefs etc.) NOW, after the
     * overlay is copied into RAM — NOT via __libc_init_array at boot (their code
     * lives in this unloaded overlay, so boot-time init hard-faults). Lynx pattern. */
    cpp_init_array(__init_array_c64_start__, __init_array_c64_end__);

    odroid_system_init(APPID_GB, 22050);
    /* Register the real save/load handlers (deferred; see c64_SaveState/c64_LoadState). */
    odroid_system_emu_init(&c64_LoadState, &c64_SaveState, NULL, NULL, NULL, NULL);

    heap_itc_alloc(true);   /* small allocs in ITCM, big spill to AXI heap (Lynx pattern) */

    /* .d64 → fast virtual 1541; .prg → no disk, RAM-injected in C64Display::Update */
    s_is_prg = (ACTIVE_FILE->ext && strcmp(ACTIVE_FILE->ext, "prg") == 0);
    ThePrefs.Emul1541Proc = false;
    if (!s_is_prg) {
        ThePrefs.DriveType[0] = DRVTYPE_D64;
        strncpy(ThePrefs.DrivePath[0], ACTIVE_FILE->path, 255);
    }
    ThePrefs.SIDType    = SIDTYPE_DIGITAL;  /* SID re-enabled (TriTable heap-alloc'd) */
    ThePrefs.SpritesOn  = true;
    ThePrefs.LimitSpeed = false;
    ThePrefs.FastReset  = true;

    /* Read the 3 ROMs STRAIGHT from SD into RAM, bypassing the flash cache entirely.
     * PROVEN on the host harness: the Frodo core + these exact BIOS files load .d64s
     * perfectly (Giana Sisters: RD t18s0 -> t18s1 -> t17 file). So the device-only
     * t18-s0 reset loop is NOT the core and NOT the ROM data — it is the ROM-LOADING
     * path. odroid_overlay_cache_file_in_flash() is a CIRCULAR buffer (gw_flash_alloc.c)
     * that can return clobbered/garbage bytes after the cache has wrapped across core
     * launches; a garbage KERNAL/BASIC makes the 6510 run junk -> MOS6510::illegal_op()
     * -> the_c64->Reset() -> drive re-Reset (the endless "RD t=18 s=0", no OPEN, no load).
     * fread is ~20KB total and avoids the cache machinery completely — deterministic. */
    static uint8 s_basic_rom[0x2000];
    static uint8 s_char_rom[0x1000];
    if (!read_bios_file("/bios/c64/basic.bin",   s_basic_rom, 0x2000) ||
        !read_bios_file("/bios/c64/chargen.bin", s_char_rom,  0x1000)) {
        return;   /* missing BIOS: bounce back to the launcher instead of freezing */
    }
    c64_ext_basic_rom = s_basic_rom;   /* RAM copies — set BEFORE `new C64` adopts them */
    c64_ext_char_rom  = s_char_rom;

    C64 *the_c64 = new C64;
    if (!read_bios_file("/bios/c64/kernal.bin", the_c64->Kernal, 0x2000)) {
        return;   /* bounce back to the launcher instead of freezing (like the other cores) */
    }
    /* Start the audio DMA BEFORE Run(): C64Display::Update() calls common_emu_sound_sync,
     * which busy-waits for the audio DMA counter to advance. Without audio_start_playing
     * the DMA never runs, dma_counter never changes, and the very first frame hangs
     * forever (the "freeze right after Run()" — sound is silent, SIDType=NONE). */
    audio_start_playing(22050 / 50);   /* PAL C64 ~50fps */
    lcd_clear_buffers();               /* black BOTH buffers once — the letterbox borders are
                                          then never redrawn per frame (see c64_repaint) */

    /* RESUME: honour the launcher's "load state" (was ignored -> saves never restored).
     * odroid_system_emu_load_state() -> c64_LoadState() records the slot path; the first
     * C64Display::Update() then runs LoadSnapshot (deferred, where TheC64 is valid). Skip
     * the LOAD/RUN autostart since the snapshot already has the game running. */
    if (load_state) {
        odroid_system_emu_load_state(save_slot);
        s_frame = 1000;                /* past the autostart triggers (frames 150/420) */
    }
    the_c64->Run();   /* blocks; per-frame work happens in C64Display::Update */
}
