// Fresh-construction save/load round-trip — replicates the DEVICE flow that the
// in-process harness (main.cxx step 5) cannot: on device, SAVE happens in one
// app session and LOAD in a SECOND session where a NEW CSystem is constructed
// from the ROM before ContextLoad runs. This catches a "load-time reconstruction
// desyncs the savestate stream" bug that the same-object round-trip misses.
//
//   build: see freshload_build.sh (same flags as run_lynx_host_test.sh)
//   run:   ./freshload_test <abs path to .lyx>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <handy.h>

// Device-only symbol pulled in by CSystem::UpdateFrame's watchdog refresh.
extern "C" void wdog_refresh(void) {}

// Device heap model: delete is a no-op (overlay bump allocator).
void operator delete(void *) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete(void *, size_t) noexcept {}
void operator delete[](void *, size_t) noexcept {}

static unsigned char *load_rom_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    const char *romPath = (argc > 1) ? argv[1] : "lynx/test.lyx";
    size_t rom_len = 0;
    unsigned char *rom = load_rom_file(romPath, &rom_len);
    if (!rom) { printf("FAILED to read ROM '%s'\n", romPath); return 2; }
    printf("ROM '%s' (%zu bytes)\n", romPath, rom_len);

    static uint16_t fb[HANDY_SCREEN_WIDTH * HANDY_SCREEN_HEIGHT];
    static SWORD    ab[HANDY_AUDIO_BUFFER_LENGTH];

    // --- SESSION 1: construct, run frames, ContextSave -----------------------
    gPrimaryFrameBuffer = (UBYTE *)fb;
    gAudioBuffer = ab;
    gAudioEnabled = 1;
    CSystem *s1 = new CSystem((const UBYTE *)rom, (ULONG)rom_len,
                              MIKIE_PIXEL_FORMAT_16BPP_565, 32000);
    if (!s1 || s1->mFileType == HANDY_FILETYPE_ILLEGAL) {
        printf("session1 construct rejected\n"); return 3;
    }
    // NOTE: synthetic fixture has no real 65C02 program, so UpdateFrame would run
    // garbage forever — skip it. The savestate STREAM structure (byte counts that
    // determine desync) is identical regardless of the state values.

    const char *savePath = "freshload.sav";
    FILE *sf = fopen(savePath, "wb");
    bool saveOk = sf && s1->ContextSave(sf);
    long bytes = sf ? ftell(sf) : -1;
    if (sf) fclose(sf);
    printf("[S1] ContextSave => %s (%ld bytes)\n", saveOk ? "OK" : "FAIL", bytes);

    // --- SESSION 2: FRESH construct from same ROM, then ContextLoad ----------
    // (device tears the first session down completely; new app launch rebuilds)
    gPrimaryFrameBuffer = (UBYTE *)fb;
    gAudioBuffer = ab;
    gAudioEnabled = 1;
    CSystem *s2 = new CSystem((const UBYTE *)rom, (ULONG)rom_len,
                              MIKIE_PIXEL_FORMAT_16BPP_565, 32000);
    if (!s2 || s2->mFileType == HANDY_FILETYPE_ILLEGAL) {
        printf("session2 construct rejected\n"); return 4;
    }
    FILE *lf = fopen(savePath, "rb");
    bool loadOk = lf && s2->ContextLoad(lf);
    if (lf) fclose(lf);
    printf("[S2] fresh ContextLoad => %s\n", loadOk ? "OK" : "FAIL");
    printf("FRESH ROUND-TRIP => %s\n", (saveOk && loadOk) ? "PASS" : "FAIL");
    return (saveOk && loadOk) ? 0 : 1;
}
