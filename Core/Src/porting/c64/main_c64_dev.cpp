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

/* Precise on-SD trace to /c64_diag.txt (delete before a clean test; capped). The last
 * line written = where the .d64 load stalled. 1541d64.cpp calls this for disk events. */
extern "C" void c64_diag(const char *fmt, ...)
{
    static int lines;
    if (lines > 600) return;
    lines++;
    FILE *f = fopen("/c64_diag.txt", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

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
    /* Auto-fit: the 40-column picture starts at VIC COL40_XSTART (0x20=32) in the 340-wide
     * Frodo bitmap — the left of that is border. Cropping at 20 (the stale Display.h note)
     * left 12px of border on the left, pushing the image right. Scale ONLY the content
     * region [32 .. DISPLAY_X) to fill the LCD width, letterboxed vertically. */
    uint16_t *out = (uint16_t *)lcd_get_active_buffer();
    const int X0 = 0x20;                                 /* content left edge (VIC COL40_XSTART) */
    const int cw = DISPLAY_X - X0;                       /* visible content columns (308) */
    int dstH = DISPLAY_Y * WIDTH / cw;                   /* fill width, keep aspect (~216) */
    if (dstH > HEIGHT) dstH = HEIGHT;
    const int y0 = (HEIGHT - dstH) / 2;
    memset(out, 0, (size_t)y0 * WIDTH * sizeof(uint16_t));
    memset(&out[(y0 + dstH) * WIDTH], 0, (size_t)(HEIGHT - y0 - dstH) * WIDTH * sizeof(uint16_t));
    for (int dy = 0; dy < dstH; dy++) {
        const uint8 *src = &s_bitmap[(dy * DISPLAY_Y / dstH) * DISPLAY_X + X0];
        uint16_t *dst = &out[(y0 + dy) * WIDTH];
        for (int dx = 0; dx < WIDTH; dx++)
            dst[dx] = s_pal565[src[dx * cw / WIDTH] & 0x0f];
    }
}

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

    c64_repaint();
    lcd_swap();          /* present the game frame; c64_repaint no longer swaps itself */

    /* Pause menu (VOLUME/SET button) -> volume / brightness / power / Quit-to-menu.
     * Custom-loop cores (Run() blocks, this is the per-frame callback) MUST call this
     * every non-warp frame with a NON-NULL repaint, else there is NO way to quit the
     * game — the user had to drain the battery. "Quit to menu" does switch_app(0). */
    odroid_gamepad_state_t js;
    odroid_input_read_gamepad(&js);
    odroid_dialog_choice_t options[] = { ODROID_DIALOG_CHOICE_LAST };
    common_emu_input_loop(&js, options, c64_repaint);
    common_emu_input_loop_handle_turbo(&js);

    if (!warp)
        common_emu_sound_sync(false);   /* during warp, don't pace to audio */
}

#ifdef __riscos__
void C64Display::PollKeyboard(uint8*,uint8*,uint8*,uint8*) {}
#else
void C64Display::PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix, uint8 *joystick)
{
    (void)key_matrix; (void)rev_matrix;
    odroid_gamepad_state_t js;
    odroid_input_read_gamepad(&js);
    uint8 m = 0xff;  /* active-low */
    if (js.values[ODROID_INPUT_UP])    m &= ~0x01;
    if (js.values[ODROID_INPUT_DOWN])  m &= ~0x02;
    if (js.values[ODROID_INPUT_LEFT])  m &= ~0x04;
    if (js.values[ODROID_INPUT_RIGHT]) m &= ~0x08;
    if (js.values[ODROID_INPUT_A])     m &= ~0x10;  /* fire */
    if (joystick) *joystick = m;
}
#endif

long ShowRequester(char *str, char *b1, char *b2) { (void)b1; (void)b2; printf("[c64] %s\n", str?str:""); return 1; }

/* ---- DigitalRenderer no-op (SIDType=NONE, never instantiated) ---- */
DigitalRenderer::DigitalRenderer() { ready = false; volume = 0; v3_mute = false; pad00 = 0; }
DigitalRenderer::~DigitalRenderer() {}
void DigitalRenderer::Reset(void) {}
void DigitalRenderer::EmulateLine(void) {}
void DigitalRenderer::WriteRegister(uint16, uint8) {}
void DigitalRenderer::NewPrefs(Prefs *) {}
void DigitalRenderer::Pause(void) {}
void DigitalRenderer::Resume(void) {}

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
    (void)load_state; (void)start_paused; (void)save_slot;

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
    ThePrefs.SIDType    = SIDTYPE_NONE;
    ThePrefs.SpritesOn  = true;
    ThePrefs.LimitSpeed = false;
    ThePrefs.FastReset  = true;
    c64_diag("=== C64 BOOT === prg=%d disk=%s\n", (int)s_is_prg, ThePrefs.DrivePath[0]);

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
        c64_diag("ROM FAIL (need /bios/c64/{basic,chargen}.bin)\n");
        return;
    }
    c64_ext_basic_rom = s_basic_rom;   /* RAM copies — set BEFORE `new C64` adopts them */
    c64_ext_char_rom  = s_char_rom;

    /* Dump ROM integrity so the log itself proves whether the ROM is good or garbage —
     * compare to the host ground-truth sums (no guessing). A mismatch = the load path
     * corrupted it; a match = ROM is fine and the bug is elsewhere (see ILLOP pc). */
    { uint32_t bs = 0, cs = 0;
      for (int i = 0; i < 0x2000; i++) bs += s_basic_rom[i];
      for (int i = 0; i < 0x1000; i++) cs += s_char_rom[i];
      c64_diag("BIOS basic sum=%08lx f=%02x%02x l=%02x%02x (want 000e3d56 94e3 00e0)\n",
               (unsigned long)bs, s_basic_rom[0], s_basic_rom[1], s_basic_rom[0x1ffe], s_basic_rom[0x1fff]);
      c64_diag("BIOS char  sum=%08lx f=%02x%02x (want 0007f7f8 3c66)\n",
               (unsigned long)cs, s_char_rom[0], s_char_rom[1]); }

    C64 *the_c64 = new C64;
    if (!read_bios_file("/bios/c64/kernal.bin", the_c64->Kernal, 0x2000)) {
        c64_diag("ROM FAIL (need /bios/c64/kernal.bin)\n");
        return;   /* bounce back to the launcher instead of freezing (like the other cores) */
    }
    { uint32_t ks = 0; for (int i = 0; i < 0x2000; i++) ks += the_c64->Kernal[i];
      c64_diag("BIOS kernal sum=%08lx resetvec=%02x%02x (want 000fc70a, e2 fc)\n",
               (unsigned long)ks, the_c64->Kernal[0x1ffc], the_c64->Kernal[0x1ffd]); }
    /* Start the audio DMA BEFORE Run(): C64Display::Update() calls common_emu_sound_sync,
     * which busy-waits for the audio DMA counter to advance. Without audio_start_playing
     * the DMA never runs, dma_counter never changes, and the very first frame hangs
     * forever (the "freeze right after Run()" — sound is silent, SIDType=NONE). */
    audio_start_playing(22050 / 50);   /* PAL C64 ~50fps */
    c64_diag("ROMs ok -> audio started -> the_c64->Run()\n");
    printf("[c64] Frodo start, disk=%s\n", ThePrefs.DrivePath[0]);
    the_c64->Run();   /* blocks; per-frame work happens in C64Display::Update */
}
