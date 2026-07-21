/* A host driver that stands exactly where main_sm.c stands: the device's source
 * set, TARGET_GNW on, snes->apu NULL, spc_player doing the audio. It is the ONLY
 * host build that can reproduce a device-only fault. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "src/types.h"
#include "src/sm_rtl.h"
#include "src/snes/snes.h"
#include "src/snes/ppu.h"
#include "src/snes/cart.h"
#include "src/snes/apu.h"
#include "src/spc_player.h"
#include "src/variables.h"
#include "src/funcs.h"

bool g_debug_flag, g_new_ppu = true, g_other_image;
int  g_got_mismatch_count;
SpcPlayer *g_spc_player;
Snes *g_snes;                 /* the one main_sm.c forgot */
bool g_use_my_apu_code = true;
bool g_fail;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void Die(const char *e) { fprintf(stderr, "DIE: %s\n", e); exit(1); }
void Warning(const char *e) { fprintf(stderr, "WARN: %s\n", e); }
void RtlDrawPpuFrame(uint8 *pb, size_t pitch, uint32 f) { (void)pb; (void)pitch; (void)f; }
void Call(uint32 a) { (void)a; }
void DebugGameOverMenu(void) {}
void RtlUpdateSnesPatchForBugfix(void) {}
uint16 currently_installed_bug_fix_counter;
void apu_reset(Apu *a) { (void)a; }
void apu_cycle(Apu *a) { (void)a; }
void apu_free(Apu *a) { (void)a; }
void apu_saveload(Apu *a, SaveLoadFunc *f, void *c) { (void)a; (void)f; (void)c; }
void ppu_copy(Ppu *a, Ppu *b) { (void)a; (void)b; }
int  CpuOpcodeHook(uint32 a) { (void)a; return 0; }
bool HookedFunctionRts(int l) { (void)l; return false; }

void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

#define SM_SRAM_SIZE 0x2000
#define FRAME_SAMPLES (16000 / 60)
static uint16_t g_fb[320 * 240];              /* RGB565, as on the device */

/* main_sm.c renders through a one-line buffer and a callback now, so the screen
 * can be scaled — and a harness that keeps pointing the PPU at the framebuffer is
 * testing a path the device no longer takes. Same program, or it proves nothing. */
static uint16_t g_line[256];

static void blit_line(unsigned y, const uint16_t *line) {
  if (y < 1 || y > 224) return;
  memcpy(g_fb + (8 + y - 1) * 320 + 32, line, 256 * sizeof(uint16_t));   /* SCALING_OFF */
}
static int16_t  g_audio[FRAME_SAMPLES];

static void RunFrame(uint16 input, int run_what) {
  (void)run_what;
  g_snes->input1->currentState = input;
  g_use_my_apu_code = true;
  g_snes->runningWhichVersion = 0xff;
  RunOneFrameOfGame();
  g_snes->hPos = g_snes->vPos = 0;
  while (!g_snes->cpu->nmiWanted) {
#ifdef SM_DOT_LOOP
    /* the old per-dot walk, kept only so the batched one can be diffed against it */
    do { snes_handle_pos_stuff(g_snes); } while (g_snes->hPos != 0);
#else
    snes_run_line(g_snes);            /* what main_sm.c calls */
#endif
    if (g_snes->vIrqEnabled && (g_snes->vPos - 1) == g_snes->vTimer) Vector_IRQ();
  }
  g_snes->cpu->nmiWanted = false;
  g_snes->runningWhichVersion = 0;
}

/* Savestate round-trip. The device saves fine and then loads to a black screen,
 * which means the state we write is not the whole state — or something outside it
 * is stale after the load. Run, save, run on; then reload and run the same frames
 * again. If the state is complete, the two runs are the same machine. */
static uint8_t g_state[512 * 1024];
static size_t  g_state_len, g_state_pos;

static void state_write(void *ctx, void *data, size_t size) {
  (void)ctx;
  memcpy(g_state + g_state_len, data, size);
  g_state_len += size;
}
static void state_read(void *ctx, void *data, size_t size) {
  (void)ctx;
  memcpy(data, g_state + g_state_pos, size);
  g_state_pos += size;
}

/* How many pixels are not the backdrop. A hash tells you two frames are the same;
 * it does not tell you they are the same BLACK. The device's symptom is a black
 * screen at full framerate, so the harness has to be able to say the word "black"
 * — otherwise "IDENTICAL" is a pass it can hand out for two identically empty
 * frames, which is exactly how this bug got past it. */
static size_t fb_lit_pixels(void) {
  size_t lit = 0;
  const uint16_t *p = (const uint16_t *)g_fb;
  for (size_t i = 0; i < sizeof(g_fb) / sizeof(uint16_t); i++)
    if (p[i] != 0) lit++;
  return lit;
}

static void ppu_report(const char *when) {
  Ppu *ppu = g_snes->ppu;
  printf("  ppu %-14s TM=%02X TS=%02X TMW=%02X TSW=%02X wsel=%06X forcedBlank=%d brightness=%d\n",
         when, ppu->screenEnabled[0], ppu->screenEnabled[1],
         ppu->screenWindowed[0], ppu->screenWindowed[1],
         (unsigned)ppu->windowsel, (int)ppu->forcedBlank, ppu->brightness);
}

static uint64_t fb_hash(void) {
  uint64_t h = 1469598103934665603ULL;
  const uint8_t *b = (const uint8_t *)g_fb;
  for (size_t i = 0; i < sizeof(g_fb); i++) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}

int main(int argc, char **argv) {
  const char *path = argv[1];
  int frames = argc > 2 ? atoi(argv[2]) : 1200;
  FILE *f = fopen(path, "rb");
  if (!f) { printf("no rom\n"); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1;
  fclose(f);

  /* exactly main_sm.c's sequence */
  g_snes = snes_init(g_ram);
  Cart *cart = g_snes->cart;
  cart->type = 1;                      /* LoROM */
  cart->rom = rom;
  cart_setRomSize(cart, (int)n);
  cart->ram = calloc(1, SM_SRAM_SIZE);
  cart->ramSize = SM_SRAM_SIZE;
  g_rom = cart->rom;
  g_sram = cart->ram;

  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);
  RtlSetupEmuCallbacks(NULL, &RunFrame, NULL);
  RtlReset(0);                         /* <-- this is what HardFaulted the device */

  for (int i = 0; i < frames; i++) {
    if (getenv("SM_KOREAN")) japanese_text_flag = 1;
    g_ppu_line_cb = &blit_line;
    PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
    RtlRunFrame(0);
    RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
  }
  /* Load a savestate FILE the device wrote. The device's black screen is not
   * reproducible from a state this harness made itself, so let it eat the real
   * one — header and all — exactly as main_sm.c does. */
  /* Where does the byte stream actually go? A savestate that disagrees with the
   * build by N bytes is misaligned from the first segment that changed size — and
   * everything after it reads garbage. Print the segments so the delta can be
   * pinned to one of them instead of guessed at. */
  if (getenv("SM_SEGMENTS")) {
    size_t mark = 0;
    #define SEG(name, call) do { g_state_len = mark; call; \
        printf("  %-22s %8zu bytes\n", name, g_state_len - mark); mark = g_state_len; } while (0)
    g_state_len = 0;
    SEG("cpu_saveload",  cpu_saveload(g_snes->cpu, &state_write, NULL));
    SEG("dma_saveload",  dma_saveload(g_snes->dma, &state_write, NULL));
    SEG("ppu_saveload",  ppu_saveload(g_snes->ppu, &state_write, NULL));
    SEG("cart_saveload", cart_saveload(g_snes->cart, &state_write, NULL));
    SEG("snes hPos..openBus", state_write(NULL, &g_snes->hPos,
            offsetof(Snes, openBus) + 1 - offsetof(Snes, hPos)));
    SEG("snes->ram",     state_write(NULL, g_snes->ram, 0x20000));
    SEG("snes->ramAdr",  state_write(NULL, &g_snes->ramAdr, 4));
    SEG("SpcPlayer",     SpcPlayer_SaveLoad(g_spc_player, &state_write, NULL));
    printf("  %-22s %8zu bytes  (file says 276275 -> delta %d)\n",
           "TOTAL", g_state_len, (int)g_state_len - 276275);
    return 0;
  }

  if (getenv("SM_LOAD_FILE")) {
    const char *sp = getenv("SM_LOAD_FILE");
    FILE *sf = fopen(sp, "rb");
    if (!sf) { printf("no savestate: %s\n", sp); return 1; }
    uint32_t hdr[3] = {0,0,0};
    if (fread(hdr, 4, 3, sf) != 3) { printf("short header\n"); return 1; }
    fseek(sf, 0, SEEK_END); long fsz = ftell(sf); fseek(sf, 12, SEEK_SET);
    printf("file: %ld bytes  magic=%08X version=%u payload_says=%u  (file has %ld after header)\n",
           fsz, hdr[0], hdr[1], hdr[2], fsz - 12);

    /* What THIS build streams, counted without writing anything. */
    g_state_len = 0;
    RtlSaveLoadState(kSaveLoad_Save, &state_write, NULL);
    printf("this build streams: %zu bytes  -> %s\n", g_state_len,
           g_state_len == hdr[2] ? "MATCHES the file"
                                 : "DOES NOT MATCH — the stream is misaligned from here on");

    g_state_len = fread(g_state, 1, sizeof(g_state), sf);
    fclose(sf);

    RtlReset(0);                       /* the launcher's resume path */
    g_state_pos = 0;
    RtlSaveLoadState(kSaveLoad_Load, &state_read, NULL);
    ppu_report("after load");

    PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
    RtlRunFrame(0);
    size_t lit = fb_lit_pixels();
    printf("first frame after loading YOUR file: %zu lit pixels of %zu -> %s\n",
           lit, sizeof(g_fb) / sizeof(uint16_t),
           lit == 0 ? "BLACK SCREEN — reproduced" : "a picture");
    return lit == 0;
  }

  if (getenv("SM_SAVELOAD")) {
    /* Capture state X, once, before anything renders past it. Also remember
     * X's brightness register value directly (not read back off the live ppu
     * later — the reference render below advances that forward) — it is what
     * the reload restores ppu->brightness to, and the poison below needs it. */
    g_state_len = 0;
    RtlSaveLoadState(kSaveLoad_Save, &state_write, NULL);
    printf("savestate: %zu bytes\n", g_state_len);
    uint8_t brightness_at_X = g_snes->ppu->brightness;
    ppu_report("at save");

    /* Reference: render the frame X -> X+1 with caches that organically match
     * cgram (nothing has poisoned or reloaded anything yet). This is the frame
     * a correct load-and-continue must reproduce. NOTE: this has to be the same
     * transition (X -> X+1) that "b" below renders — comparing this frame to
     * a LATER one (e.g. hashing 60 frames post-load instead of the very next
     * one) is comparing two different game frames on principle, which will
     * differ whether or not the cache bug exists. That mistake is what let
     * this exact bug ship three times. */
    PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
    RtlRunFrame(0);
    RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
    uint64_t a = fb_hash();
    size_t lit_a = fb_lit_pixels();

    /* Earlier version of this harness tried to reach a "different palette" PPU
     * state by driving the game elsewhere with button input before reloading,
     * then diffing the framebuffer some frames later. Both halves of that were
     * too weak to catch the bug: pressing Start for a few hundred frames does
     * not reliably land on a visibly different palette before the harness's
     * frame budget runs out, and ordinary gameplay (HUD flashes, palette
     * cycling, the NMI handler) rewrites cgram within a handful of frames
     * regardless of the bug, healing a stale cache before a later-frame hash
     * ever sees it. It ran IDENTICAL clean at every frame count tried, on both
     * fixed and reverted ppu_saveload().
     *
     * So: poison the caches directly instead. First, SM_COLD_LOAD (which models
     * what the device does after a firmware update: boot, launch, load a state
     * immediately) — has to run BEFORE the poisoning below, because RtlReset()
     * runs ppu_init(), which memsets the whole Ppu (palette565, paletteDirty,
     * lastBrightnessMult included) to 0. Poisoning first would just get thrown
     * away here, and would test nothing. */
    if (getenv("SM_COLD_LOAD")) {
      RtlReset(0);
      printf("(cold: the PPU has drawn nothing yet, as after a reboot)\n");
    }

    /* Then poison the caches on the state that is about to be overwritten by
     * the reload below. This is exactly what "drive the game elsewhere, then
     * load" leaves behind — palette565/brightnessMult holding a different
     * scene's colours while cgram is about to become X's again — done without
     * hoping a button sequence gets there in time. paletteDirty = false and
     * lastBrightnessMult == brightness_at_X both assert "these caches already
     * match the state that is about to be loaded" — the exact lie
     * ppu_saveload's fix has to overrule. (lastBrightnessMult must equal what
     * Load() is about to restore ppu->brightness to, brightness_at_X, not some
     * other value — ppu_runLine() has its OWN brightness!=lastBrightnessMult
     * check that rebuilds brightnessMult independently of the load path, and a
     * mismatched poison gets "fixed" by THAT check regardless of whether
     * ppu_saveload's fix exists, which proves nothing.) */
    Ppu *ppu = g_snes->ppu;
    for (int i = 0; i < 256; i++) ppu->palette565[i] = (uint16_t)~ppu->palette565[i];
    memset(ppu->brightnessMult, 0x55, sizeof(ppu->brightnessMult));
    ppu->paletteDirty = false;
    ppu->lastBrightnessMult = brightness_at_X;

    /* Reload state X. Restores cgram (and everything else ppu_saveload streams)
     * back to X's values, underneath whatever the caches above were just set
     * to — the same shape the real bug takes: load restores state, nothing
     * tells the derived caches. */
    g_state_pos = 0;
    RtlSaveLoadState(kSaveLoad_Load, &state_read, NULL);

    /* Same transition as the reference (X -> X+1, from the state the reload
     * just restored), so a correct load must reproduce hash a exactly. */
    PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
    RtlRunFrame(0);
    RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
    uint64_t b = fb_hash();
    size_t lit_b = fb_lit_pixels();
    ppu_report("after load");

    printf("  lit pixels: reference %zu   after load %zu   (of %zu)\n",
           lit_a, lit_b, sizeof(g_fb) / sizeof(uint16_t));

    /* Two ways to fail, and the harness used to be able to see only one.
     * 1. The frames differ  -> something the load did not restore.
     * 2. The frames MATCH and both are black -> the game was on a black screen
     *    when we saved, so the round-trip proves nothing at all. Say so instead
     *    of printing a pass. */
    if (lit_a == 0) {
      printf("save/load round-trip: VACUOUS — the reference frame is already black, "
             "so this run compares nothing. Save from a frame with a picture on it.\n");
      return 2;
    }
    if (a != b) {
      printf("save/load round-trip: DIFFERENT — the load did not restore everything "
             "the renderer reads%s\n",
             lit_b == 0 ? " (and the loaded frame is BLACK — the device's symptom)" : "");
      return 1;
    }
    printf("save/load round-trip: IDENTICAL (%zu lit pixels, so this was a real picture)\n", lit_b);
    return 0;
  }

  /* FNV-1a over everything the frame produced: the screen, the game's RAM and its
   * save RAM. Two builds that claim to emulate the same machine must agree here. */
  uint64_t h = 1469598103934665603ULL;
  #define HASH(p, n) do { const uint8_t *b_ = (const uint8_t *)(p); \
    for (size_t i_ = 0; i_ < (size_t)(n); i_++) { h ^= b_[i_]; h *= 1099511628211ULL; } } while (0)
  HASH(g_fb, sizeof(g_fb));
  HASH(g_ram, 0x20000);
  HASH(g_sram, SM_SRAM_SIZE);
  printf("device-reality run: %d frames OK (apu=%p, must be NULL) state=%016llx\n",
         frames, (void *)g_snes->apu, (unsigned long long)h);
  return 0;
}
