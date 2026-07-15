/* Super Metroid NATIVE PORT on QEMU's Cortex-M7 (mps2-an500): executed-instruction
 * counts per frame for the hand-decompiled SM port — NOT the generic interpreter
 * core. SM's vehicle is main_sm.c + sm_rtl.c + the shared snes/*.c; this measures
 * that, so today's shared-ppu.c optimizations show up in SM's ledger.
 *
 * Frame loop + globals copied from tools/sm_harness/device_main.c (the device's
 * TARGET_GNW reality: snes->apu NULL, spc_player is the sound chip). Timing /
 * calibration / windowing copied from tools/m7_qemu_rig/rig_snes.c.
 *
 * Split: emu (RtlRunFrame = SM game logic + PPU compositing) vs aud (RtlRenderAudio
 * = spc_player sequencer + DSP). -icount shift=0 makes a CMSDK-timer delta an
 * instruction count. insn != cycle, QEMU models no caches — device is the judge;
 * the RATIO across an A/B is the reliable part.
 */
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

#ifndef RIG_FRAMES
#define RIG_FRAMES 1200
#endif
#ifndef RIG_WINDOW
#define RIG_WINDOW 200
#endif

/* rig_runtime_hf.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

extern unsigned char _binary_rom_smc_start[];
extern unsigned char _binary_rom_smc_end[];

/* --- the globals + stubs the SM port stands on (from device_main.c) --- */
bool g_debug_flag, g_new_ppu = true, g_other_image;
int  g_got_mismatch_count;
SpcPlayer *g_spc_player;
Snes *g_snes;
bool g_use_my_apu_code = true;
bool g_fail;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void Die(const char *e) { printf("DIE: %s\n", e); exit(1); }
void Warning(const char *e) { (void)e; }
void RtlDrawPpuFrame(uint8 *pb, size_t pitch, uint32 f) { (void)pb; (void)pitch; (void)f; }
void Call(uint32 a) { (void)a; }
void DebugGameOverMenu(void) {}
void RtlUpdateSnesPatchForBugfix(void) {}
uint16 currently_installed_bug_fix_counter;
void apu_reset(Apu *a) { (void)a; }
void apu_cycle(Apu *a) { (void)a; }
void apu_run(Apu *a, int c) { (void)a; (void)c; }
void apu_free(Apu *a) { (void)a; }
void apu_saveload(Apu *a, SaveLoadFunc *f, void *c) { (void)a; (void)f; (void)c; }
void ppu_copy(Ppu *a, Ppu *b) { (void)a; (void)b; }
int  CpuOpcodeHook(uint32 a) { (void)a; return 0; }
bool HookedFunctionRts(int l) { (void)l; return false; }
/* SM's sm_rtl.c / spc_player.c reference fopen (savestate file I/O) which drags in
 * these newlib syscalls; rig_runtime_hf.c provides the rest but not these. Never
 * called in the rig (no file ops in the measurement loop) — stubbed to satisfy ld. */
int _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; return -1; }
int _unlink(const char *p) { (void)p; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; return -1; }

void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

#define SM_SRAM_SIZE  0x2000
#define FRAME_SAMPLES (16000 / 60)
static uint16_t g_fb[320 * 240];
static uint16_t g_line[256];
static int16_t  g_audio[FRAME_SAMPLES];

static void blit_line(unsigned y, const uint16_t *line) {
  if (y < 1 || y > 224) return;
  memcpy(g_fb + (8 + y - 1) * 320 + 32, line, 256 * sizeof(uint16_t));
}

/* device_main.c's RunFrame, verbatim: the callback RtlRunFrame drives. */
static void RunFrame(uint16 input, int run_what) {
  (void)run_what;
  g_snes->input1->currentState = input;
  g_use_my_apu_code = true;
  g_snes->runningWhichVersion = 0xff;
  RunOneFrameOfGame();
  g_snes->hPos = g_snes->vPos = 0;
  while (!g_snes->cpu->nmiWanted) {
    snes_run_line(g_snes);
    if (g_snes->vIrqEnabled && (g_snes->vPos - 1) == g_snes->vTimer) Vector_IRQ();
  }
  g_snes->cpu->nmiWanted = false;
  g_snes->runningWhichVersion = 0;
}

static uint64_t fnv1a(const void *d, size_t n) {
  uint64_t h = 1469598103934665603ULL; const uint8_t *p = d;
  while (n--) { h ^= *p++; h *= 1099511628211ULL; } return h;
}

int main(void) {
  unsigned char *rom = _binary_rom_smc_start;
  uint32_t rom_len = (uint32_t)(_binary_rom_smc_end - _binary_rom_smc_start);
  uint32_t hdr = (rom_len % 1024 == 512) ? 512 : 0;
  rom += hdr; rom_len -= hdr;

  /* FPU on (CPACR CP10/CP11) before any double op — hard-float build. */
  *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
  __asm__ volatile("dsb; isb");
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[sm-qemu] boot\n");
  rig_timer_init();
  uint32_t cal = rig_calibrate(1000000);
  uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal ? cal : 1));
  printf("[sm-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
         (unsigned long)cal, (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

  /* exactly main_sm.c / device_main.c's setup sequence */
  g_snes = snes_init(g_ram);
  Cart *cart = g_snes->cart;
  cart->type = 1;                        /* LoROM */
  cart->rom = rom;
  cart_setRomSize(cart, (int)rom_len);
  cart->ram = calloc(1, SM_SRAM_SIZE);
  cart->ramSize = SM_SRAM_SIZE;
  g_rom = cart->rom;
  g_sram = cart->ram;
  g_spc_player = SpcPlayer_Create();
  SpcPlayer_Initialize(g_spc_player);
  RtlSetupEmuCallbacks(NULL, &RunFrame, NULL);
  RtlReset(0);
  printf("[sm-qemu] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

  uint64_t run_hash = 1469598103934665603ULL;
  uint64_t win_emu = 0, win_aud = 0, tot_emu = 0, tot_aud = 0;

  for (int frame = 0; frame < RIG_FRAMES; frame++) {
#ifdef RIG_INPUT_TAP
    uint16 input = (frame >= 40 && (frame % 24) < 6) ? 0x0008 : 0;   /* tap Start */
#else
    uint16 input = 0;
#endif
    g_ppu_line_cb = &blit_line;
#ifdef RIG_FRAMESKIP
    g_ppu_skip_render = true;   /* skip PPU compositing; render-on minus this = PPU share */
#endif
    PpuBeginDrawing(g_snes->ppu, (uint8_t *)g_line, 0, 0);

    uint32_t t0 = rig_timer_now();
    RtlRunFrame(input);
    uint32_t t1 = rig_timer_now();
    RtlRenderAudio(g_audio, FRAME_SAMPLES, 1);
    uint32_t t2 = rig_timer_now();
    win_emu += (uint32_t)(t1 - t0);
    win_aud += (uint32_t)(t2 - t1);

    uint64_t h = fnv1a(g_fb, sizeof(g_fb));
    run_hash = (run_hash ^ h) * 1099511628211ULL;

    if ((frame + 1) % RIG_WINDOW == 0) {
      uint64_t e = win_emu * ipt_x1000 / 1000 / RIG_WINDOW;
      uint64_t a = win_aud * ipt_x1000 / 1000 / RIG_WINDOW;
      int lit = 0; for (int q = 0; q < 320 * 240; q++) if (g_fb[q]) lit++;
      printf("w%05d emu=%lu aud=%lu insn/frame fb=%08lx lit=%d\n",
             frame + 1, (unsigned long)e, (unsigned long)a,
             (unsigned long)(uint32_t)h, lit);
      tot_emu += win_emu; tot_aud += win_aud; win_emu = win_aud = 0;
    }
  }
  uint64_t fr = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW; if (!fr) fr = 1;
  printf("[sm-qemu] done %d frames STATEHASH=%08lx avg emu=%lu aud=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)run_hash,
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / fr),
         (unsigned long)(tot_aud * ipt_x1000 / 1000 / fr));
  return 0;
}
