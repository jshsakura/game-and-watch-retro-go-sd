// Atari Lynx (Handy) HOST test harness.
//
// Headless, fact-driven diagnosis of the Lynx ROM-load path. Mirrors the
// linux/a2600 host-build pattern but needs no SDL window (pure stdout log).
//
// It exercises exactly the device-side decisions from
// Core/Src/porting/lynx/main_lynx.cpp:
//   1. Load a .lyx fixture via malloc + fread (host stand-in for getromdata's
//      odroid_overlay_cache_file_in_flash XIP pointer).
//   2. new CSystem(...) and read the data-derived mFileType.
//   3. The exact device acceptance check
//        (lynx == NULL || mFileType == HANDY_FILETYPE_ILLEGAL).
//   4. Replicate odroid_system_get_path_buf() SAVE_STATE logic for a normal
//      "/roms/lynx/test.lyx" path (does it PANIC?).
//   5. ContextSave -> ContextLoad round-trip fidelity.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <handy.h>   // pulls in system.h (CSystem, HANDY_FILETYPE_*, MIKIE_*)

// The device's overlay C++ heap (heap.cpp) is a bump allocator whose
// operator delete[] is a NO-OP. Model that here so ~CCart deleting the XIP
// (flash-resident) bank0 pointer is harmless exactly like on device — under
// glibc it would otherwise bad-free a non-malloc'd pointer. (Run with
// ASAN_OPTIONS=detect_leaks=0; a short-lived test leaking is fine.)
void operator delete(void *) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete(void *, size_t) noexcept {}
void operator delete[](void *, size_t) noexcept {}

// ---------------------------------------------------------------------------
// Faithful replica of odroid_system_get_path_buf() (Core/Src/porting/
// odroid_system.c) for the SAVE_STATE case. We replicate rather than link the
// device file to keep the harness self-contained (build isolation). Constants
// match retro-go-stm32/components/odroid/config.h:
//   ODROID_BASE_PATH_ROMS  = "/roms"
//   ODROID_BASE_PATH_SAVES = "/data"
// and emu_path_type_t ODROID_PATH_SAVE_STATE == 0.
// ---------------------------------------------------------------------------
#define HOST_BASE_PATH_ROMS  "/roms"
#define HOST_BASE_PATH_SAVES "/data"
#define HOST_PATH_SAVE_STATE 0

// Returns true if a real path was produced (no panic), false if the device
// would have hit RG_PANIC("Invalid ROM path!").
static bool host_get_save_state_path(const char *romPath, char *out, int out_size)
{
    const char *fileName = romPath;

    if (strstr(fileName, HOST_BASE_PATH_ROMS))
    {
        fileName = strstr(fileName, HOST_BASE_PATH_ROMS);
        fileName += strlen(HOST_BASE_PATH_ROMS);
    }

    if (!fileName || strlen(fileName) < 4)
    {
        // Device only escapes the panic for homebrew; a normal lynx path here
        // would PANIC. Report that instead of aborting the harness.
        return false;
    }

    snprintf(out, out_size, "%s%s-%d.sav",
             HOST_BASE_PATH_SAVES, fileName, HOST_PATH_SAVE_STATE);
    return true;
}

static const char *filetype_name(ULONG t)
{
    switch (t)
    {
        case HANDY_FILETYPE_LNX:      return "HANDY_FILETYPE_LNX";
        case HANDY_FILETYPE_HOMEBREW: return "HANDY_FILETYPE_HOMEBREW";
        case HANDY_FILETYPE_SNAPSHOT: return "HANDY_FILETYPE_SNAPSHOT";
        case HANDY_FILETYPE_ILLEGAL:  return "HANDY_FILETYPE_ILLEGAL";
        case HANDY_FILETYPE_RAW:      return "HANDY_FILETYPE_RAW";
        default:                      return "UNKNOWN";
    }
}

// Host stand-in for getromdata(): malloc + fread the whole .lyx file. CSystem /
// CCart copy (or XIP-reference) what they need, so a heap buffer is sufficient.
static unsigned char *load_rom_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0)
    {
        fclose(fp);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz)
    {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    const char *romFsPath = (argc > 1) ? argv[1] : "lynx/test.lyx";
    // The device path the launcher would hand to odroid_system_get_path_buf.
    const char *deviceRomPath = "/roms/lynx/test.lyx";

    printf("===== Atari Lynx (Handy) HOST harness =====\n");

    // 1. Load fixture ROM (malloc + fread) -----------------------------------
    size_t rom_len = 0;
    unsigned char *rom_ptr = load_rom_file(romFsPath, &rom_len);
    printf("[1] ROM load: path='%s'\n", romFsPath);
    if (!rom_ptr)
    {
        printf("    FAILED to read fixture ROM (run linux/update_lynx_rom.sh "
               "and launch from the linux/ dir, or pass a path).\n");
        return 1;
    }
    printf("    rom_ptr=%p rom_len=%zu bytes\n", (void *)rom_ptr, rom_len);
    printf("    first 4 bytes: %02X %02X %02X %02X ('%c%c%c%c')\n",
           rom_ptr[0], rom_ptr[1], rom_ptr[2], rom_ptr[3],
           rom_ptr[0], rom_ptr[1], rom_ptr[2], rom_ptr[3]);

    // 2. Construct CSystem ---------------------------------------------------
    printf("[2] new CSystem(rom, %zu, MIKIE_PIXEL_FORMAT_16BPP_565, 32000)...\n",
           rom_len);
    CSystem *lynx = new CSystem((const UBYTE *)rom_ptr, (ULONG)rom_len,
                                MIKIE_PIXEL_FORMAT_16BPP_565, 32000);
    printf("    CSystem @%p\n", (void *)lynx);

    // 3. mFileType + the EXACT device acceptance check -----------------------
    ULONG ft = lynx ? lynx->mFileType : (ULONG)HANDY_FILETYPE_ILLEGAL;
    printf("[3] mFileType = %lu (%s)\n", (unsigned long)ft, filetype_name(ft));
    bool rejected = (lynx == NULL || lynx->mFileType == HANDY_FILETYPE_ILLEGAL);
    printf("    device check (lynx==NULL || mFileType==HANDY_FILETYPE_ILLEGAL)"
           " => %s\n", rejected ? "REJECTED" : "ACCEPTED");
    if (lynx)
        printf("    cart name='%s' manuf='%s'\n",
               lynx->mCart->CartGetName(), lynx->mCart->CartGetManufacturer());

    // 4. Save-state path build (odroid_system_get_path_buf replica) ----------
    char pathBuf[256];
    bool ok = host_get_save_state_path(deviceRomPath, pathBuf, sizeof(pathBuf));
    printf("[4] save-state path for romPath='%s'\n", deviceRomPath);
    if (ok)
        printf("    produced: '%s'  (no panic)\n", pathBuf);
    else
        printf("    would PANIC: \"Invalid ROM path!\"\n");

    // 5. ContextSave -> ContextLoad round-trip -------------------------------
    printf("[5] ContextSave -> ContextLoad round-trip\n");
    if (!lynx || rejected)
    {
        printf("    skipped (no valid CSystem)\n");
    }
    else
    {
        const char *savePath = "lynx_harness.sav";
        FILE *sf = fopen(savePath, "wb");
        bool saveOk = sf && lynx->ContextSave(sf);
        long bytesWritten = -1;
        if (sf)
        {
            bytesWritten = ftell(sf);
            fclose(sf);
        }
        printf("    ContextSave => %s (%ld bytes written to '%s')\n",
               saveOk ? "OK" : "FAIL", bytesWritten, savePath);

        FILE *lf = fopen(savePath, "rb");
        bool loadOk = lf && lynx->ContextLoad(lf);
        if (lf)
            fclose(lf);
        printf("    ContextLoad => %s\n", loadOk ? "OK" : "FAIL");
        printf("    round-trip   => %s\n",
               (saveOk && loadOk) ? "PASS" : "FAIL");
    }

    // 6. RUN EMULATION FRAMES — the device main loop the old harness skipped.
    //    Exercises Susie/Mikie/65C02 + the XIP bank0 reads; under ASan this
    //    catches OOB reads past the (flash-resident) ROM and any write OOB.
    if (lynx && !rejected)
    {
        static uint16_t fb[HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT]; /* DEVICE-FAITHFUL 160x102: catches any fb WRITE OOB the device would suffer */
        static SWORD    ab[HANDY_AUDIO_BUFFER_LENGTH];
        gPrimaryFrameBuffer = (UBYTE *)fb;
        gAudioBuffer = ab;
        gAudioEnabled = 1;
        int frames = (argc > 2) ? atoi(argv[2]) : 600;
        printf("[6] running %d emulation frames (UpdateFrame)...\n", frames);
        for (int i = 0; i < frames; i++)
        {
            lynx->SetButtonData(0);
            lynx->UpdateFrame(true);
            gAudioBufferPointer = 0;
        }
        printf("    %d frames completed, no crash\n", frames);
    }

    delete lynx;
    free(rom_ptr);
    printf("===== harness done =====\n");
    return 0;
}

// Device glue references wdog_refresh(); provide a no-op so any shared object
// that pulls main.h is satisfied (unused by this headless harness).
extern "C" void wdog_refresh(void) {}
