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
  if (getenv("SM_SAVELOAD")) {
    /* save here */
    g_state_len = 0;
    RtlSaveLoadState(kSaveLoad_Save, &state_write, NULL);
    printf("savestate: %zu bytes\n", g_state_len);

    /* run 60 frames from the save point and remember what the screen became */
    for (int i = 0; i < 60; i++) {
      PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
      RtlRunFrame(0);
      RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
    }
    uint64_t a = fb_hash();

    /* reload and run the same 60 frames.
     *
     * SM_COLD_LOAD models what the device actually does after a firmware update:
     * boot, launch, load a state immediately. The PPU has rendered nothing yet, so
     * everything it caches is still zero — and a load restores cgram without
     * touching any of that. */
    if (getenv("SM_COLD_LOAD")) {
      RtlReset(0);
      printf("(cold: the PPU has drawn nothing yet, as after a reboot)\n");
    }
    g_state_pos = 0;
    RtlSaveLoadState(kSaveLoad_Load, &state_read, NULL);
    for (int i = 0; i < 60; i++) {
      PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);
      RtlRunFrame(0);
      RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
    }
    uint64_t b = fb_hash();

    int lit = 0;
    for (int i = 0; i < 320 * 240; i++) if (g_fb[i]) lit++;
    printf("save/load round-trip: %s   (lit after reload: %d/76800)\n",
           a == b ? "IDENTICAL" : "DIFFERENT — the state is incomplete", lit);
    return a != b;
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
