/* Host harness for the WonderSwan core (oswan) — mirrors the DEVICE build:
 * same WSWAN_C_SOURCES, same -DGNW_WSWAN/-DNOSDL_FB/-DSOUND_ON defines, same
 * software renderer. Three modes:
 *
 *   cross   — straight run + RUNHASH, to cross-check the QEMU M7 rig (identical
 *             ROM + input script must give identical per-frame FrameBuffer
 *             hashes; a divergence means a host-vs-target UB).
 *
 *   record  — run to `total`, snapshot the savestate at `save_frame`, and write
 *   resume    per-frame reference IRAM+FB hashes. `resume` then runs in a FRESH
 *             process (a real device COLD boot: every static at its true reset
 *             value, not warmed by a prior run), loads the snapshot, and checks
 *             each post-load frame against the reference. IRAM match == the game
 *             state resumes exactly; FB-only mismatch == cosmetic.
 *
 * The two-process split is what makes `resume` faithful to the device: a single
 * process carries statics across passes and hides cold-boot-only bugs. This rig
 * reproduced (and gated the fix for) the One Piece Grand Battle resume hang —
 * MemDummy scratch not captured by the savestate.
 *
 *   ./retro-go-wswan <rom> [total] [save_frame] [cross|record|resume]
 *   WS_WARMUP=N  (resume) run N frames before loading — mimics the web/SDL front
 *                end's warm load; N=0 is the device's cold load (default).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- oswan core entry points (forward-declared as main_wswan.c does) ---- */
void     WsInit(void);
void     WsReset(void);
uint32_t WsRun(void);
int      ws_create_from_flash(const uint8_t *data, uint32_t size);
uint32_t WsSaveStateToFile(FILE *fp);
uint32_t WsLoadStateFromFile(FILE *fp);

extern uint16_t FrameBuffer[240 * 144];
extern int      ws_render_enabled;
extern uint8_t  IRAM[];

/* ---- device-glue stubs, identical to main_wswan.c's no-ops ---- */
char gameName[512] = "/tmp/wsrig/game";
void graphics_paint(void)  { }
void Sound_APU_Start(void) { }
void Sound_APU_End(void)   { }
void Sound_APUClose(void)  { }
void Pause_Sound(void)     { }

/* ---- deterministic input script — MUST stay byte-identical to
 *      tools/m7_qemu_rig/rig_wswan.c so the two rigs produce comparable hashes. */
static int s_frame;
uint32_t WsInputGetState(void)
{
    uint32_t k = 0;
    if (s_frame >= 60  && s_frame < 76)  k |= 0x0200;            /* START */
    if (s_frame >= 200 && (s_frame % 90) < 8) k |= 0x0400;      /* A tap */
    if (s_frame >= 400 && (s_frame % 300) < 8) k |= 0x0020;     /* X2 right nudge */
    return k;
}

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    while (len--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static uint8_t *load_file(const char *path, uint32_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n);
    if (fread(buf, 1, n, fp) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(fp);
    *out_len = (uint32_t)n;
    return buf;
}

/* Fresh machine from the same ROM image (device launch sequence minus WsInit,
 * which we call once at process start). */
static void machine_reset(const uint8_t *rom, uint32_t rom_len)
{
    if (!ws_create_from_flash(rom, rom_len)) {
        fprintf(stderr, "ws_create_from_flash FAILED\n");
        exit(3);
    }
    WsReset();
}

#define STATE_PATH   "/tmp/ws_harness_state.bin"
#define REFHASH_PATH "/tmp/ws_harness_ref.bin"

static void run_cross(const uint8_t *rom, uint32_t rom_len, int total)
{
    machine_reset(rom, rom_len);
    uint32_t run_hash = 2166136261u;
    for (s_frame = 0; s_frame < total; s_frame++) {
        ws_render_enabled = 1;
        WsRun();
        uint32_t h = fnv1a(FrameBuffer, sizeof(FrameBuffer));
        run_hash = (run_hash ^ h) * 16777619u;
        if ((s_frame + 1) % 200 == 0)
            printf("[ws-host] f%05d fb=%08x\n", s_frame + 1, h);
    }
    printf("[ws-host] cross done %d frames RUNHASH=%08x\n", total, run_hash);
}

static int do_record(const uint8_t *rom, uint32_t rom_len, int total, int save_frame)
{
    machine_reset(rom, rom_len);
    FILE *ref = fopen(REFHASH_PATH, "wb");
    if (!ref) { fprintf(stderr, "cannot write ref\n"); return 2; }
    fwrite(&total, 4, 1, ref); fwrite(&save_frame, 4, 1, ref);
    for (s_frame = 0; s_frame < total; s_frame++) {
        ws_render_enabled = 1;
        WsRun();
        if (s_frame == save_frame) {
            FILE *fp = fopen(STATE_PATH, "wb");
            uint32_t err = fp ? WsSaveStateToFile(fp) : 1;
            if (fp) fclose(fp);
            printf("[ws-host] record: saved state at frame %d (err=%u)\n", save_frame, err);
        }
        if (s_frame > save_frame) {
            uint32_t ih = fnv1a(IRAM, 0x10000);
            uint32_t fh = fnv1a(FrameBuffer, sizeof(FrameBuffer));
            fwrite(&ih, 4, 1, ref); fwrite(&fh, 4, 1, ref);
        }
    }
    fclose(ref);
    printf("[ws-host] record: refs written for frames %d..%d\n", save_frame + 1, total - 1);
    return 0;
}

static int do_resume(const uint8_t *rom, uint32_t rom_len)
{
    FILE *ref = fopen(REFHASH_PATH, "rb");
    if (!ref) { fprintf(stderr, "no ref (run record first)\n"); return 2; }
    int total = 0, save_frame = 0;
    if (fread(&total, 4, 1, ref) != 1 || fread(&save_frame, 4, 1, ref) != 1) return 2;

    machine_reset(rom, rom_len);
    int warmup = getenv("WS_WARMUP") ? atoi(getenv("WS_WARMUP")) : 0;
    for (s_frame = 0; s_frame < warmup; s_frame++) { ws_render_enabled = 1; WsRun(); }

    FILE *fp = fopen(STATE_PATH, "rb");
    uint32_t load_err = fp ? WsLoadStateFromFile(fp) : 1;
    if (fp) fclose(fp);
    printf("[ws-host] resume: warmup=%d load_err=%u frames %d..%d\n",
           warmup, load_err, save_frame + 1, total - 1);

    int fb_mism = 0, iram_mism = 0, iram_first = -1, fb_first = -1;
    for (s_frame = save_frame + 1; s_frame < total; s_frame++) {
        ws_render_enabled = 1;
        WsRun();
        uint32_t ih = fnv1a(IRAM, 0x10000);
        uint32_t fh = fnv1a(FrameBuffer, sizeof(FrameBuffer));
        uint32_t rih = 0, rfh = 0;
        if (fread(&rih, 4, 1, ref) != 1 || fread(&rfh, 4, 1, ref) != 1) break;
        if (ih != rih) { if (iram_first < 0) iram_first = s_frame; iram_mism++; }
        if (fh != rfh) { if (fb_first < 0) fb_first = s_frame; fb_mism++; }
    }
    fclose(ref);

    printf("\n========= WS SAVESTATE COLD ROUND-TRIP (2-process) =========\n");
    printf("  post-load frames  : %d\n", total - save_frame - 1);
    printf("  FB mismatches     : %d  (first frame %d)\n", fb_mism, fb_first);
    printf("  IRAM mismatches   : %d  (first frame %d)  <- game state\n", iram_mism, iram_first);
    printf("  VERDICT           : %s\n",
           iram_mism == 0 ? "PASS (game state resumes exactly on a cold boot)"
                          : "FAIL (cold resume diverges -> bug)");
    printf("============================================================\n");
    return iram_mism == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom> [total] [save_frame] [cross|record|resume]\n", argv[0]);
        return 1;
    }
    uint32_t rom_len = 0;
    uint8_t *rom = load_file(argv[1], &rom_len);
    int total = argc > 2 ? atoi(argv[2]) : 1200;
    int save_frame = argc > 3 ? atoi(argv[3]) : -1;
    const char *mode = argc > 4 ? argv[4] : "cross";
    (void)save_frame;

    WsInit();
    int rc = 0;
    if      (!strcmp(mode, "record")) rc = do_record(rom, rom_len, total, save_frame);
    else if (!strcmp(mode, "resume")) rc = do_resume(rom, rom_len);
    else                              run_cross(rom, rom_len, total);
    free(rom);
    return rc;
}
