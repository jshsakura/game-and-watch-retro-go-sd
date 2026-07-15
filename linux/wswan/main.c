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
#include <time.h>

/* Deterministic RTC: oswan's ReadIO reads the wall clock for the RTC port, so a
 * game that touches the RTC gives a different result every run (and host != rig).
 * Override time() to a fixed value so base-vs-idle host gating is reproducible. */
time_t time(time_t *t) { if (t) *t = 0; return 0; }

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

#ifdef WS_IDLE_LOG
/* Record which PCs the idle-skip fires on, so a wrongly-skipped loop can be
 * disassembled. */
static struct { uint32_t pc, n; } idle_log[64];
extern uint8_t *Page[16];
void ws_idle_log(uint32_t pc)
{
    for (int i = 0; i < 64; i++) {
        if (idle_log[i].pc == pc) { idle_log[i].n++; return; }
        if (idle_log[i].n == 0)   {
            idle_log[i].pc = pc; idle_log[i].n = 1;
            /* dump the 16 bytes of the loop the first time this PC fires */
            uint32_t lin = ((pc >> 16) << 4) + (pc & 0xFFFF);
            uint8_t *bank = Page[(lin >> 16) & 0xF];
            printf("  [loop @ %04x:%04x lin=%05x] ", pc >> 16, pc & 0xFFFF, lin);
            for (int b = -8; b < 12; b++) printf("%02x ", bank[(lin & 0xFFFF) + b]);
            printf("\n");
            return;
        }
    }
}
static void ws_idle_log_dump(void)
{
    printf("=== idle-skip fired at these PCs ===\n");
    for (int i = 0; i < 64 && idle_log[i].n; i++)
        printf("  %04x:%04x  x%u\n", idle_log[i].pc >> 16, idle_log[i].pc & 0xFFFF, idle_log[i].n);
}
#endif

static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    while (len--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* Hash only the VISIBLE screen (cols 8..231, the 224px the blit shows) — the
 * FrameBuffer margins (cols 0-7 / 232-239) are never displayed and hold render
 * spill / stale bytes that differ harmlessly, so a full-buffer hash mis-flags
 * the idle-skip as diverging when the screen is pixel-identical. */
static uint32_t fnv_visible(const uint16_t *fb)
{
    uint32_t h = 2166136261u;
    for (int row = 0; row < 144; row++)
        for (int col = 8; col < 232; col++) {
            uint16_t px = fb[row * 240 + col];
            h ^= px & 0xff; h *= 16777619u; h ^= px >> 8; h *= 16777619u;
        }
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
    int dump_from = getenv("WS_DUMP_FROM") ? atoi(getenv("WS_DUMP_FROM")) : -1;
    uint32_t run_hash = 2166136261u;
    for (s_frame = 0; s_frame < total; s_frame++) {
        ws_render_enabled = 1;
        WsRun();
        uint32_t h = fnv_visible(FrameBuffer);
        run_hash = (run_hash ^ h) * 16777619u;
        if (dump_from >= 0 && s_frame >= dump_from)
            printf("  f%05d fb=%08x iram=%08x\n", s_frame, h, fnv1a(IRAM, 0x10000));
        if (getenv("WS_FBDUMP") && s_frame == atoi(getenv("WS_FBDUMP"))) {
            FILE *f = fopen(getenv("WS_FBOUT"), "wb");
            if (f) { fwrite(FrameBuffer, sizeof(FrameBuffer), 1, f); fclose(f); }
        }
        if ((s_frame + 1) % 200 == 0)
            printf("[ws-host] f%05d fb=%08x\n", s_frame + 1, h);
    }
    printf("[ws-host] cross done %d frames RUNHASH=%08x\n", total, run_hash);
#ifdef WS_IDLE_LOG
    ws_idle_log_dump();
#endif
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

#ifdef WS_PC_HIST
/* PC histogram: run `total` frames and report which linear PCs the V30 spent
 * them on. A tight cluster that dominates is a spin/idle-wait loop. */
extern uint32_t *ws_pc_hist;
extern int       ws_pc_hist_on;
static int do_hist(const uint8_t *rom, uint32_t rom_len, int total, int warm)
{
    ws_pc_hist = (uint32_t *)calloc((size_t)1 << 21, sizeof(uint32_t));
    machine_reset(rom, rom_len);
    /* Warm up to `warm` frames un-counted (skip boot/logo), then count. */
    for (s_frame = 0; s_frame < warm; s_frame++) { ws_render_enabled = 1; WsRun(); }
    ws_pc_hist_on = 1;
    for (; s_frame < total; s_frame++) { ws_render_enabled = 1; WsRun(); }
    ws_pc_hist_on = 0;

    uint64_t tot = 0;
    for (size_t i = 0; i < (size_t)1 << 21; i++) tot += ws_pc_hist[i];

    /* top-24 hottest PCs */
    printf("\n===== WS PC HISTOGRAM (%d frames after %d warmup) =====\n", total - warm, warm);
    printf("  total counted instrs: %llu  (~%llu/frame)\n",
           (unsigned long long)tot, (unsigned long long)(tot / (total - warm ? total - warm : 1)));
    printf("  rank  linearPC   hits        %%\n");
    uint64_t top_sum = 0; uint32_t lo = 0xFFFFFFFF, hi = 0;
    for (int r = 0; r < 24; r++) {
        size_t best = 0; uint32_t bestv = 0;
        for (size_t i = 0; i < (size_t)1 << 21; i++)
            if (ws_pc_hist[i] > bestv) { bestv = ws_pc_hist[i]; best = i; }
        if (!bestv) break;
        double pct = tot ? 100.0 * bestv / tot : 0;
        printf("  %2d    %05zx     %-10u  %5.2f%%\n", r + 1, best, bestv, pct);
        top_sum += bestv;
        if (r < 16) { if ((uint32_t)best < lo) lo = best; if ((uint32_t)best > hi) hi = best; }
        ws_pc_hist[best] = 0; /* consume so next rank finds the next */
    }
    printf("  ----\n");
    printf("  top-24 PCs = %.1f%% of all executed instructions\n", tot ? 100.0 * top_sum / tot : 0);
    printf("  top-16 span linear %05x..%05x (%u bytes) — a tight span = one hot loop\n", lo, hi, hi - lo);
    printf("=========================================================\n");
    free(ws_pc_hist); ws_pc_hist = 0;
    return 0;
}
#endif

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
#ifdef WS_PC_HIST
    else if (!strcmp(mode, "hist"))   rc = do_hist(rom, rom_len, total, save_frame < 0 ? 300 : save_frame);
#endif
    else                              run_cross(rom, rom_len, total);
    free(rom);
    return rc;
}
