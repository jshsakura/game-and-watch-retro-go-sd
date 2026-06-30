/* Headless host harness glue for Frodo (frodo-go core).
 * Replaces the ESP32 Display/audio/main with PC stubs:
 *  - C64Display: holds the 384x272 8bpp bitmap + palette, dumps c64f_frame.ppm
 *  - autostart: types LOAD"*",8,1 / RUN into the C64 keyboard buffer to load .d64
 *  - DigitalRenderer: no-op (SIDType=NONE, never instantiated, just a vtable stub)
 * Build: make -f Makefile.c64frodo ; run: ./build/c64frodo game.d64 [frames]
 * Needs Basic.rom/Kernal.rom/Char.rom in cwd. */
#include "sysdeps.h"
#include "C64.h"
#include "Display.h"
#include "Prefs.h"
#include "DigitalRenderer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <new>

/* host_glue uses the REAL fopen/fclose for its own bookkeeping (ROM loads, PPM dump);
 * the device-faithful 1-open-file limit (h_fopen, esp_shim.h) applies to the vendored
 * Frodo sources — the device code path that the .d64 + any diag actually run through. */
#undef fopen
#undef fclose
static int g_open_files = 0;
extern "C" FILE *h_fopen(const char *path, const char *mode)
{
    if (g_open_files >= 1) {
        fprintf(stderr, "\n[HARNESS DEVICE-FAITHFUL] BUG: 2nd fopen('%s') while %d file "
                "already open. The device allows only ONE (gw_littlefs MAX_OPEN_FILES=1) "
                "and corrupts the first handle here — exactly the c64_diag/.d64 loop.\n",
                path, g_open_files);
        abort();
    }
    FILE *f = fopen(path, mode);
    if (f) g_open_files++;
    return f;
}
extern "C" int h_fclose(FILE *f) { if (f) g_open_files--; return fclose(f); }

/* ---- DEVICE-FAITHFUL bounded heap ----------------------------------------
 * The device C64 overlay has a ~100KB run-time heap (badheap). Model it here so
 * device OOM reproduces on the host (plain malloc was unbounded). Overflow aborts
 * with the size — exactly what the device's `assert(heap offset <= heapsize)` does. */
#define DEVICE_HEAP_SIZE 102712            /* = RAM_END - _OVERLAY_C64_BSS_END on device */
static unsigned char g_dev_heap[DEVICE_HEAP_SIZE];
static size_t        g_dev_heap_off = 0;
extern "C" void *dev_heap_alloc(size_t sz)
{
    size_t a = (sz + 3u) & ~(size_t)3u;
    if (g_dev_heap_off + a > DEVICE_HEAP_SIZE) {
        fprintf(stderr, "[DEV-HEAP OOM] alloc=%zu used=%zu/%d  -> DEVICE WOULD ASSERT HERE\n",
                sz, g_dev_heap_off, DEVICE_HEAP_SIZE);
        fflush(stderr);
        abort();
    }
    void *p = &g_dev_heap[g_dev_heap_off];
    g_dev_heap_off += a;
    return p;
}
extern "C" size_t dev_heap_used(void) { return g_dev_heap_off; }
void *operator new(std::size_t s)   { return dev_heap_alloc(s); }
void *operator new[](std::size_t s) { return dev_heap_alloc(s); }
void  operator delete(void *)   noexcept {}
void  operator delete[](void *) noexcept {}
void  operator delete(void *, std::size_t)   noexcept {}
void  operator delete[](void *, std::size_t) noexcept {}

/* IsFrodoSC is defined in C64.cpp */

/* ---- standard C64 16-colour palette (RGB) for PPM output ---- */
static const uint8 c64rgb[16][3] = {
    {0x00,0x00,0x00},{0xff,0xff,0xff},{0x68,0x37,0x2b},{0x70,0xa4,0xb2},
    {0x6f,0x3d,0x86},{0x58,0x8d,0x43},{0x35,0x28,0x79},{0xb8,0xc7,0x6f},
    {0x6f,0x4f,0x25},{0x43,0x39,0x00},{0x9a,0x67,0x59},{0x44,0x44,0x44},
    {0x6c,0x6c,0x6c},{0x9a,0xd2,0x84},{0x6c,0x5e,0xb5},{0x95,0x95,0x95},
};

static char     g_d64[512];
static int      g_maxframes = 600;
static int      g_frame = 0;

/* ===== C64Display ===== */
static uint8 s_bitmap[DISPLAY_X * DISPLAY_Y];
static uint8 s_colmap[256];

C64Display::C64Display(C64 *the_c64) : TheC64(the_c64)
{
    quit_requested = false;
    memset(s_bitmap, 0, sizeof(s_bitmap));
}
C64Display::~C64Display() {}

uint8 *C64Display::BitmapBase(void) { return s_bitmap; }
int    C64Display::BitmapXMod(void) { return DISPLAY_X; }
void   C64Display::UpdateLEDs(int,int,int,int) {}
void   C64Display::Speedometer(int) {}
bool   C64Display::NumLock(void) { return false; }
void   C64Display::NewPrefs(Prefs *) {}

void C64Display::InitColors(uint8 *colors)
{
    for (int i = 0; i < 256; i++) colors[i] = i & 0x0f;   /* identity: bitmap holds 0..15 */
    memcpy(s_colmap, colors, 256);
}

/* feed a PETSCII string into the C64 keyboard buffer ($0277, count $C6, max 10) */
static const char *s_typing = NULL;
static int s_type_pos = 0;
static void feed_keyboard(uint8 *ram)
{
    if (!s_typing) return;
    if (ram[0xC6] != 0) return;                 /* wait until buffer drained */
    int n = 0;
    while (s_typing[s_type_pos] && n < 10) {
        ram[0x0277 + n] = (uint8)s_typing[s_type_pos];
        s_type_pos++; n++;
    }
    ram[0xC6] = (uint8)n;
    if (!s_typing[s_type_pos]) { s_typing = NULL; s_type_pos = 0; }
}

void C64Display::Update(void)
{
    g_frame++;
    uint8 *ram = TheC64->RAM;
    /* autostart state machine */
    if (g_frame == 150)      { s_typing = "LOAD\"*\",8,1\r"; s_type_pos = 0; }
    else if (g_frame == 360) { s_typing = "RUN\r";          s_type_pos = 0; }
    if (g_frame >= 150) feed_keyboard(ram);

    if (g_frame >= g_maxframes) {
        FILE *p = fopen("c64f_frame.ppm", "wb");
        fprintf(p, "P6\n%d %d\n255\n", DISPLAY_X, DISPLAY_Y);
        for (int i = 0; i < DISPLAY_X * DISPLAY_Y; i++) {
            const uint8 *c = c64rgb[s_bitmap[i] & 0x0f];
            fputc(c[0], p); fputc(c[1], p); fputc(c[2], p);
        }
        fclose(p);
        printf("[c64f] wrote c64f_frame.ppm %dx%d after %d frames\n", DISPLAY_X, DISPLAY_Y, g_frame);
        exit(0);
    }
}

#ifdef __riscos__
void C64Display::PollKeyboard(uint8*,uint8*,uint8*,uint8*) {}
#else
void C64Display::PollKeyboard(uint8 *key_matrix, uint8 *rev_matrix, uint8 *joystick)
{
    /* after the game's RUN, hold FIRE + SPACE so crack intros advance into the game */
    if (joystick) *joystick = (g_frame > 700) ? 0xef : 0xff;  /* bit4=fire (active low) */
    if (key_matrix && rev_matrix && g_frame > 700) {
        key_matrix[7] &= ~0x10;   /* SPACE = row7 col4 (active low) */
        rev_matrix[4] &= ~0x80;
    }
}
#endif

long ShowRequester(char *str, char *b1, char *b2)
{
    (void)b1; (void)b2;
    printf("[c64f] ShowRequester: %s\n", str ? str : "(null)");
    return 1;
}

/* ===== DigitalRenderer no-op stub (SIDType=NONE so never instantiated) ===== */
DigitalRenderer::DigitalRenderer() { ready = false; volume = 0; v3_mute = false; pad00 = 0; }
DigitalRenderer::~DigitalRenderer() {}
void DigitalRenderer::Reset(void) {}
void DigitalRenderer::EmulateLine(void) {}
void DigitalRenderer::WriteRegister(uint16, uint8) {}
void DigitalRenderer::NewPrefs(Prefs *) {}
void DigitalRenderer::Pause(void) {}
void DigitalRenderer::Resume(void) {}

/* ===== ROM loading ===== */
static bool load_rom(const char *path, uint8 *dst, int want)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("[c64f] missing %s\n", path); return false; }
    int n = (int)fread(dst, 1, want, f);
    fclose(f);
    if (n != want) { printf("[c64f] %s wrong size %d!=%d\n", path, n, want); return false; }
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1) { strncpy(g_d64, argv[1], sizeof(g_d64)-1); }
    if (argc > 2) g_maxframes = atoi(argv[2]);

    /* preferences: fast virtual 1541 from a .d64, no SID renderer */
    ThePrefs.Emul1541Proc = false;
    ThePrefs.DriveType[0] = DRVTYPE_D64;
    strncpy(ThePrefs.DrivePath[0], g_d64, 255);
    ThePrefs.SIDType    = SIDTYPE_NONE;
    ThePrefs.SpritesOn  = true;
    ThePrefs.LimitSpeed = false;
    ThePrefs.FastReset  = true;

    /* Mirror the device: read-only Basic/Char are provided externally (no heap alloc
     * inside C64::C64()); only the patched Kernal is copied into the C64's own buffer. */
    static uint8 s_basic[0x2000], s_char[0x1000];
    if (!load_rom("Basic.rom", s_basic, 0x2000) ||
        !load_rom("Char.rom",  s_char,  0x1000)) return 1;
    c64_ext_basic_rom = s_basic;
    c64_ext_char_rom  = s_char;

    C64 *the_c64 = new C64;

    if (!load_rom("Kernal.rom", the_c64->Kernal, 0x2000)) return 1;

    printf("[c64f] disk=%s frames=%d\n", g_d64, g_maxframes);
    the_c64->Run();   /* blocks; Display::Update() dumps + exits at g_maxframes */
    return 0;
}
