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

void C64Display::Update(void)
{
    s_frame++;
    wdog_refresh();

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

    /* blit Frodo bitmap -> LCD (320 wide crop, RGB565), letterboxed into 240 rows */
    uint16_t *out = (uint16_t *)lcd_get_inactive_buffer();
    memset(out, 0, (size_t)C64_LETTERBOX_Y * WIDTH * sizeof(uint16_t));
    memset(&out[(C64_LETTERBOX_Y + DISPLAY_Y) * WIDTH], 0,
           (size_t)(HEIGHT - C64_LETTERBOX_Y - DISPLAY_Y) * WIDTH * sizeof(uint16_t));
    for (int y = 0; y < DISPLAY_Y; y++) {
        const uint8 *src = &s_bitmap[y * DISPLAY_X + C64_CROP_X];
        uint16_t *dst = &out[(C64_LETTERBOX_Y + y) * WIDTH];
        for (int x = 0; x < 320; x++) dst[x] = s_pal565[src[x] & 0x0f];
    }
    lcd_swap();
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

extern "C" void app_main_c64(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;

    /* Run the Frodo C++ static constructors (global ThePrefs etc.) NOW, after the
     * overlay is copied into RAM — NOT via __libc_init_array at boot (their code
     * lives in this unloaded overlay, so boot-time init hard-faults). Lynx pattern. */
    cpp_init_array(__init_array_c64_start__, __init_array_c64_end__);

    odroid_system_init(APPID_GB, 22050);

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

    /* Basic/Char MUST be COPIED into RAM, not used live from the flash cache.
     * odroid_overlay_cache_file_in_flash() is a CIRCULAR buffer (gw_flash_alloc.c):
     * a later cache write (chargen, kernal, or any other core's ROM/logo cached on a
     * previous launch) wraps and OVERWRITES an earlier entry. When Basic/Char pointed
     * straight at flash, that clobber turned the BASIC ROM into garbage -> the 6510 ran
     * junk -> MOS6510::illegal_op() called the_c64->Reset() every time -> the drive was
     * re-Reset in a loop (the endless "RD t=18 s=0" with no OPEN, never loading the .d64).
     * It "worked once" only when the cache happened not to have wrapped yet. Copying each
     * ROM into a private RAM buffer right after caching makes it immune to later writes.
     * 12KB of overlay BSS is a fine trade for not corrupting the ROM. Kernal is likewise
     * copied (it must be writable for C64::PatchKernal's IEC hooks). */
    static uint8 s_basic_rom[0x2000];
    static uint8 s_char_rom[0x1000];
    if (!load_rom("/bios/c64/basic.bin",   s_basic_rom, 0x2000) ||
        !load_rom("/bios/c64/chargen.bin", s_char_rom,  0x1000)) {
        c64_diag("ROM FAIL (need /bios/c64/{basic,chargen}.bin)\n");
        return;
    }
    c64_ext_basic_rom = s_basic_rom;   /* RAM copies — set BEFORE `new C64` adopts them */
    c64_ext_char_rom  = s_char_rom;

    C64 *the_c64 = new C64;
    if (!load_rom("/bios/c64/kernal.bin", the_c64->Kernal, 0x2000)) {
        c64_diag("ROM FAIL (need /bios/c64/kernal.bin)\n");
        return;   /* bounce back to the launcher instead of freezing (like the other cores) */
    }
    /* Start the audio DMA BEFORE Run(): C64Display::Update() calls common_emu_sound_sync,
     * which busy-waits for the audio DMA counter to advance. Without audio_start_playing
     * the DMA never runs, dma_counter never changes, and the very first frame hangs
     * forever (the "freeze right after Run()" — sound is silent, SIDType=NONE). */
    audio_start_playing(22050 / 50);   /* PAL C64 ~50fps */
    c64_diag("ROMs ok -> audio started -> the_c64->Run()\n");
    printf("[c64] Frodo start, disk=%s\n", ThePrefs.DrivePath[0]);
    the_c64->Run();   /* blocks; per-frame work happens in C64Display::Update */
}
