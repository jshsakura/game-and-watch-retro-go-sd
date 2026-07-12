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
    PpuBeginDrawing(g_snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    RtlRunFrame(0);
    RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
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
