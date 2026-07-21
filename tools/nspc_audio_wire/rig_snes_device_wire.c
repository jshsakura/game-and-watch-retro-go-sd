/* QEMU Cortex-M7 rig for the DEVICE's own N-SPC audio-HLE wire
 * (tools/nspc_audio_wire/nspc_wire.c) -- as opposed to run_snes_wire.sh /
 * rig_snes_wire.c, which link tools/nspc_audio_wire/wire.c, the host
 * proof-of-concept copy. The two source files are independent
 * implementations of the same swap idea (see nspc_wire.c's own comment
 * "Differences from the host proof-of-concept"); this rig exists so a
 * device-only defect in nspc_wire.c reproduces here instead of first
 * showing up on a user's hardware. See CLAUDE.md's "Testing a core the way
 * the device runs it" for why compiling wire.c is not a test of nspc_wire.c.
 *
 * Event loop and firmware-allocator shims are the checked-in pattern from
 * tools/m7_qemu_rig/rig_snes_hle.c (itself "verbatim from rig_snes.c /
 * snes_main.c") -- not copied from any generated/untracked file. The only
 * addition over that template is routing $2140-43 traffic through the wire
 * (wire_apu_write/wire_try_swap/wire_frame_audio) instead of straight into
 * apu->inPorts, in the same per-frame order Core/Src/porting/snes/main_snes.c
 * uses: wire_try_swap() fires at the END of run_frame_events() (after
 * snes_catchupApu), and the audio pull (wire_frame_audio when g_wire_on,
 * else the plain dsp_getSamples fallback) happens AFTER that, once per
 * frame in main(). Getting this order right matters: wire_try_swap can flip
 * g_wire_on mid-loop, and the audio pull for that same frame must see the
 * post-swap state, exactly as the firmware does.
 *
 * ROM linked in via objcopy -I binary (symbols _binary_rom_smc_start/end).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"

#ifndef RIG_FRAMES
#define RIG_FRAMES 1200
#endif
#ifndef RIG_WINDOW
#define RIG_WINDOW 200
#endif

bool snes_loadRom(Snes *snes, const uint8_t *data, int length);

/* tools/m7_qemu_rig/rig_runtime_hf.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

extern unsigned char _binary_rom_smc_start[];
extern unsigned char _binary_rom_smc_end[];

/* ---- firmware allocators (rig heap via _sbrk) ---- */
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

int  CpuOpcodeHook(uint32_t addr) { (void)addr; return 0; }
bool HookedFunctionRts(int level) { (void)level; return false; }
bool g_fail;
bool g_new_ppu = true;
void Die(const char *s) { printf("Die: %s\n", s); exit(1); }
void Warning(const char *s) { (void)s; }

/* ---- the device wire's own API (Core/Src/porting/snes/main_snes.c's
 * extern-declaration pattern -- no shared header exports these) ---- */
extern int g_wire_on, g_wire_enable;
extern const char *g_wire_variant;
void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val);
int  wire_try_swap(Snes *snes, int frame);
void wire_frame_audio(int16_t *buf, int n);
bool wire_configure_rom(const uint8_t *rom, uint32_t len);

static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  wire_apu_write(g_the_snes, adr, val);   /* wire_apu_write() does its own catchup */
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[16000 / 60];
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---- event loop, verbatim from rig_snes_hle.c / snes_main.c ---- */
static int dots_to_next_event(Snes *snes) {
  int h = snes->hPos;
  if (h == 0 || h == 512 || h == 1024) return 0;
  if (snes->hIrqEnabled && h == snes->hTimer * 4) return 0;
  int next = 1362;
  if (h < 512)       next = 512;
  else if (h < 1024) next = 1024;
  if (snes->hIrqEnabled) {
    int t = snes->hTimer * 4;
    if (t > h && t < next) next = t;
  }
  return next - h;
}
static void apply_irq_match(Snes *snes) {
  if (!(snes->hIrqEnabled || snes->vIrqEnabled)) return;
  if (snes->vIrqEnabled && snes->vPos != snes->vTimer) return;
  if (snes->hIrqEnabled && snes->hPos != snes->hTimer * 4) return;
  snes->inIrq = true;
  snes->cpu->irqWanted = true;
}
static void cpu_tick(Snes *snes) {
  if (dma_cycle(snes->dma)) return;
  if (snes->cpuCyclesLeft == 0) {
    snes->cpuMemOps = 0;
    int cycles = cpu_runOpcode(snes->cpu);
    snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  }
  snes->cpuCyclesLeft -= 2;
}
static void run_dots(Snes *snes, int dots) {
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma);
      snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (snes->cpuCyclesLeft == 0) {
      apply_irq_match(snes);
      snes->cpuMemOps = 0;
      int cycles = cpu_runOpcode(snes->cpu);
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
    }
    int step;
    if (snes->cpuCyclesLeft >= 2 && !started_dma) {
      step = snes->cpuCyclesLeft;
      if (step > dots) step = dots;
      step &= ~1;
      snes->cpuCyclesLeft -= (uint8_t)step;
    } else {
      step = 2;
      snes->cpuCyclesLeft -= 2;
    }
    snes->apuCatchupCycles += apuCyclesPerMaster * step;
    snes->hPos += step; dots -= step;
  }
}
/* Same as rig_snes_hle.c's run_frame_events, PLUS the wire_try_swap() call
 * main_snes.c makes at the same spot (end of frame, after snes_catchupApu),
 * so detection sees the same ARAM-settledness a real frame boundary gives it. */
static void run_frame_events(Snes *snes, int frame) {
  for (;;) {
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);
    cpu_tick(snes);
    if (snes->hPos == 0 && snes->vPos == 0) break;
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
  wire_try_swap(snes, frame);
}

static uint64_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ULL;
  while (len--) { h ^= *p++; h *= 1099511628211ULL; }
  return h;
}

int main(void) {
  unsigned char *raw = _binary_rom_smc_start;
  uint32_t raw_len = (uint32_t)(_binary_rom_smc_end - _binary_rom_smc_start);
  uint32_t hdr = (raw_len % 1024 == 512) ? 512 : 0;
  unsigned char *rom = raw + hdr;
  uint32_t rom_len = raw_len - hdr;

  /* Enable the FPU (CPACR CP10/CP11) before any FP op -- hard-float build,
   * matches rig_runtime_hf's Reset_Handler NOT already doing this (VB rig is
   * soft-float, this one must self-enable). */
  *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
  __asm__ volatile("dsb; isb");

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[nspc-device-wire] boot\n");
  rig_timer_init();
  uint32_t cal_ticks = rig_calibrate(1000000);
  uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
  printf("[nspc-device-wire] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
         (unsigned long)cal_ticks,
         (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
#ifdef WIRE_OFF
  g_wire_enable = 0;
#endif
  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\n"); return 1; }
  wire_configure_rom(rom, rom_len);
#ifdef WIRE_OFF
  g_wire_enable = 0;
#endif
#if !defined(GNW_SNES_CORE)
  /* Host builds: the loader malloc'd a pow2 copy; reuse the linked-in image.
   * GNW_SNES_CORE builds: cart_load already points at `rom` IN PLACE (zero-copy)
   * -- freeing it here would free the ROM itself. */
  free(snes->cart->rom);
  snes->cart->rom = rom;
#endif
  printf("[nspc-device-wire] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

  uint64_t run_hash = 1469598103934665603ULL;
  uint64_t audio_hash = 1469598103934665603ULL;
  uint64_t win_emu = 0, win_apu = 0, tot_emu = 0, tot_apu = 0;
  int last_wire_on = 0;

  for (int frame = 0; frame < RIG_FRAMES; frame++) {
#ifdef RIG_INPUT_TAP
    /* Periodic Start, only to walk a title screen into gameplay for
     * measurement runs -- changes state, so never for an input=0 cross-check. */
    snes->input1->currentState = (frame >= 40 && (frame % 24) < 6) ? 0x0008 : 0;
#else
    snes->input1->currentState = 0;
#endif
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);

    uint32_t t0 = rig_timer_now();
    run_frame_events(snes, frame);
    uint32_t t1 = rig_timer_now();

    /* Same audio-pull order and condition as main_snes.c's snes_pcm_submit(). */
    if (snes->apu) {
      if (g_wire_on) {
        wire_frame_audio(g_audio, 16000 / 60);
      } else {
        while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
        dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
      }
    } else {
      memset(g_audio, 0, sizeof(g_audio));
    }
    uint32_t t2 = rig_timer_now();
    win_emu += (uint32_t)(t1 - t0);
    win_apu += (uint32_t)(t2 - t1);

    if (g_wire_on != last_wire_on) {
      printf("[nspc-device-wire] frame %d: g_wire_on %d -> %d (variant=%s)\n",
             frame, last_wire_on, g_wire_on, g_wire_variant);
      last_wire_on = g_wire_on;
    }

    uint64_t h = fnv1a(g_fb, sizeof(g_fb));
    run_hash = (run_hash ^ h) * 1099511628211ULL;
    uint64_t ah = fnv1a(g_audio, sizeof(g_audio));
    audio_hash = (audio_hash ^ ah) * 1099511628211ULL;

    if ((frame + 1) % RIG_WINDOW == 0) {
      uint64_t emu_i = win_emu * ipt_x1000 / 1000 / RIG_WINDOW;
      uint64_t apu_i = win_apu * ipt_x1000 / 1000 / RIG_WINDOW;
      int lit = 0;
      for (int q = 0; q < 320 * 240; q++) if (g_fb[q]) lit++;
      printf("w%05d emu=%lu apu=%lu insn/frame fb=%08lx audio=%08lx lit=%d wire_on=%d\n",
             frame + 1, (unsigned long)emu_i, (unsigned long)apu_i,
             (unsigned long)(uint32_t)h, (unsigned long)(uint32_t)ah, lit, g_wire_on);
      tot_emu += win_emu; tot_apu += win_apu;
      win_emu = win_apu = 0;
    }
  }
  uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
  if (frames == 0) frames = 1;
  uint64_t sh = fnv1a(g_wram, sizeof(g_wram)) ^ fnv1a(snes->cart->ram, snes->cart->ramSize);
  printf("[nspc-device-wire] done %d frames STATEHASH=%08lx AUDIOHASH=%08lx avg emu=%lu apu=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(uint32_t)audio_hash,
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));
  printf("[nspc-device-wire] wire_on=%d variant=%s\n", g_wire_on, g_wire_variant);
  return 0;
}
