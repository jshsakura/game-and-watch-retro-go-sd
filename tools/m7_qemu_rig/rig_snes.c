/* Generic SNES core on the M7 QEMU rig: real ARMv7-M instruction counts per
 * frame, split emu vs audio. Same core sources (external/sm/src/snes), same
 * GNW_SNES_CORE branches and RGB565 PPU as the device — so a CMSDK-timer delta
 * under -icount shift=0 is an executed-instruction count.
 *
 * The event loop and glue are copied from tools/snes_harness/snes_main.c so the
 * two rigs run the same machine; the state hash printed here matches that
 * harness for the same ROM + frame count (input held at 0 both places).
 *
 * ROM linked in (objcopy -I binary): symbols _binary_rom_smc_start/end.
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
#ifdef SNES_SPIN_SKIP
#include "src/snes/spin_skip.h"
#endif

#ifndef RIG_FRAMES
#define RIG_FRAMES 1200
#endif
#ifndef RIG_WINDOW
#define RIG_WINDOW 200
#endif

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

#ifndef RIG_ROM_LOADER
extern unsigned char _binary_rom_smc_start[];
extern unsigned char _binary_rom_smc_end[];
#endif

#ifdef RIG_COST_PROF
uint64_t g_cpu_ticks, g_spc_ticks, g_dsp_ticks, g_dsp_calls;
static uint64_t g_active_voice_sum, g_echo_voice_sum, g_echo_write_frames;
static uint64_t g_present_ticks;
#define PROFILE_CPU(expr) ({ \
  uint32_t _ct = rig_timer_now(); \
  int _cycles = (expr); \
  g_cpu_ticks += (uint32_t)(rig_timer_now() - _ct); \
  _cycles; \
})
#else
#define PROFILE_CPU(expr) (expr)
#endif

#ifdef RIG_PPU_DEEP
uint64_t g_ppu_bg_ticks[3];
uint64_t g_ppu_sprite_eval_ticks, g_ppu_sprite_draw_ticks;
uint64_t g_ppu_clear_ticks, g_ppu_palette_ticks;
uint64_t g_ppu_fast_ticks, g_ppu_math_ticks, g_ppu_line_ticks;
uint64_t g_ppu_fast_pixels, g_ppu_math_pixels;
#endif

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

extern bool g_ppu_skip_render;   /* frameskip: skip PPU compositing, keep logic */
static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[16000 / 60];
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

#ifdef RIG_DEVICE_VIDEO
static uint16_t g_line[256];
static uint16_t g_device_frame[320 * 240];
static uint16_t g_lcd_frame[320 * 240];
static void rig_blit_line(unsigned y, const uint16_t *line) {
  if (y < 1) return;
  unsigned row = (y - 1) + 8;
  if (row < 240) {
#ifdef RIG_PPU_DEEP
    uint32_t _lt = rig_timer_now();
#endif
    memcpy(g_device_frame + row * 320 + 32, line, sizeof(g_line));
#ifdef RIG_PPU_DEEP
    __asm__ volatile("" :: "r"(g_device_frame) : "memory");
    g_ppu_line_ticks += (uint32_t)(rig_timer_now() - _lt);
#endif
  }
}
#endif

/* ---- event loop, verbatim from snes_main.c ---- */
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

#ifdef SNES_SPIN_SKIP
static int run_one_opcode(Snes *snes) {
  Cpu *cpu = snes->cpu;
  uint32_t pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
  int dispatch = (cpu->nmiWanted || (cpu->irqWanted && !cpu->i) || cpu->waiting) &&
                 !cpu->stopped;
  snes->cpuMemOps = 0;
  int cycles = PROFILE_CPU(cpu_runOpcode(cpu));
  snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  g_spin.ops_real++;
  spin_note(cpu, pc24, (uint8_t)snes->cpuCyclesLeft, dispatch);
  return cycles;
}
#endif

static void cpu_tick(Snes *snes) {
  if (dma_cycle(snes->dma)) return;
  if (snes->cpuCyclesLeft == 0) {
#ifdef SNES_SPIN_SKIP
    run_one_opcode(snes);
#else
    snes->cpuMemOps = 0;
    int cycles = PROFILE_CPU(cpu_runOpcode(snes->cpu));
    snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
#endif
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
#ifdef SNES_SPIN_SKIP
      Cpu *cpu = snes->cpu;
      if (g_spin.on &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !snes->hIrqEnabled &&
          !(snes->vIrqEnabled && snes->vPos == snes->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == g_spin.pc[g_spin.idx]) {
        snes->cpuCyclesLeft += g_spin.charge[g_spin.idx];
        g_spin.idx = (g_spin.idx + 1) % g_spin.len;
        cpu->k = (uint8_t)(g_spin.pc[g_spin.idx] >> 16);
        cpu->pc = (uint16_t)g_spin.pc[g_spin.idx];
        g_spin.ops_virtual++;
      } else {
        apply_irq_match(snes);
        run_one_opcode(snes);
        started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
      }
#else
      apply_irq_match(snes);
      snes->cpuMemOps = 0;
      int cycles = PROFILE_CPU(cpu_runOpcode(snes->cpu));
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
#endif
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
static void run_frame_events(Snes *snes) {
  for (;;) {
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);
    cpu_tick(snes);
    if (snes->hPos == 0 && snes->vPos == 0) break;
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
#ifdef SNES_SPIN_SKIP
  spin_frame_tick();
#endif
}

static uint64_t fnv1a(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ULL;
  while (len--) { h ^= *p++; h *= 1099511628211ULL; }
  return h;
}

int main(void) {
#ifdef RIG_ROM_LOADER
  /* The batch runner injects a ROM and its little-endian length directly into
   * otherwise-unused MPS2 PSRAM with QEMU's generic loader.  The core ELF is
   * therefore built once and reused for thousands of cartridges. */
  unsigned char *raw = (unsigned char *)0x60800000u;
  uint32_t raw_len = *(volatile uint32_t *)0x607ffffcu;
  if (raw_len == 0 || raw_len > 0x800000u) {
    printf("[snes-qemu] invalid injected ROM length=%lu\n", (unsigned long)raw_len);
    return 2;
  }
#else
  unsigned char *raw = _binary_rom_smc_start;
  uint32_t raw_len = (uint32_t)(_binary_rom_smc_end - _binary_rom_smc_start);
#endif
  uint32_t hdr = (raw_len % 1024 == 512) ? 512 : 0;
  unsigned char *rom = raw + hdr;
  uint32_t rom_len = raw_len - hdr;

  /* Enable the FPU (CPACR CP10/CP11) — we build hard-float fpv5-d16 to match the
   * device, but rig_runtime's Reset_Handler doesn't touch CPACR (the VB rig is
   * soft-float). Without this the core's first double op takes a NOCP fault into
   * Default_Handler's for(;;) and the rig hangs silently. Must precede any FP. */
  *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
  __asm__ volatile("dsb; isb");

  setvbuf(stdout, NULL, _IONBF, 0);   /* newlib block-buffers to a non-tty; flush now */
  printf("[snes-qemu] boot\n");
  rig_timer_init();
  uint32_t cal_ticks = rig_calibrate(1000000);
  uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
  printf("[snes-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
         (unsigned long)cal_ticks,
         (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
#ifdef SNES_SPIN_SKIP
  spin_whitelist_set(rom, rom_len);
  spin_reset();
#endif
  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\n"); return 1; }
  /* snes_loadRom malloc'd a second copy of the ROM. The ELF already carries the
   * blob in PSRAM — point cart->rom back at it and free the copy, so a 6 MB cart
   * costs 6 MB, not 12. The core only reads cart->rom; SRAM writes go to cart->ram.
   * Rig-only: the device streams the ROM from SD, it never double-stores. */
#if !defined(GNW_SNES_CORE)
  /* Host builds: the loader malloc'd a pow2 copy; reuse the linked-in image.
   * GNW_SNES_CORE builds: cart_load already points at `rom` IN PLACE (zero-copy)
   * — freeing it here would free the ROM itself. */
  free(snes->cart->rom);
  snes->cart->rom = rom;
#endif
  printf("[snes-qemu] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

  uint64_t run_hash = 1469598103934665603ULL;
  uint64_t audio_hash = 1469598103934665603ULL;   /* per-frame g_audio fold (audio-path gate) */
  uint64_t win_emu = 0, win_apu = 0, tot_emu = 0, tot_apu = 0;

  for (int frame = 0; frame < RIG_FRAMES; frame++) {
#ifdef RIG_INPUT_TAP
    /* Tap Start (bit 3) periodically to walk title/menus into gameplay; only for
     * measurement runs (changes state, so not for the input=0 host cross-check). */
    snes->input1->currentState = (frame >= 40 && (frame % 24) < 6) ? 0x0008 : 0;
#else
    snes->input1->currentState = 0;
#endif
#ifdef RIG_DEVICE_VIDEO
    g_ppu_line_cb = &rig_blit_line;
    PpuBeginDrawing(snes->ppu, (uint8_t *)g_line, 0, 0);
#else
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
#endif

#ifdef RIG_FRAMESKIP
    g_ppu_skip_render = true;   /* measure CPU+APU+timing without PPU compositing;
                                 * render-on minus this = the PPU's share of a frame */
#endif
    uint32_t t0 = rig_timer_now();
    run_frame_events(snes);
    uint32_t t1 = rig_timer_now();
#ifdef RIG_DEVICE_VIDEO
    uint32_t tp = rig_timer_now();
    memcpy(g_lcd_frame, g_device_frame, sizeof(g_lcd_frame));
    __asm__ volatile("" :: "r"(g_lcd_frame) : "memory");
    g_present_ticks += (uint32_t)(rig_timer_now() - tp);
    uint32_t ta = rig_timer_now();
#else
    uint32_t ta = t1;
#endif
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
    }
    uint32_t t2 = rig_timer_now();
    win_emu += (uint32_t)(t1 - t0);
    win_apu += (uint32_t)(t2 - ta);

#ifdef RIG_DEVICE_VIDEO
    uint64_t h = fnv1a(g_device_frame, sizeof(g_device_frame));
#else
    uint64_t h = fnv1a(g_fb, sizeof(g_fb));
#endif
    run_hash = (run_hash ^ h) * 1099511628211ULL;

#ifdef RIG_COST_PROF
    if (snes->apu) {
      unsigned active = 0, echo = 0;
      for (int ch = 0; ch < 8; ch++) {
        DspChannel *c = &snes->apu->dsp->channel[ch];
        if (c->gain != 0 || c->adsrState != 4) active++;
        if (c->echoEnable) echo++;
      }
      g_active_voice_sum += active;
      g_echo_voice_sum += echo;
      if (snes->apu->dsp->echoWrites) g_echo_write_frames++;
    }
#endif

    /* Audio-path gate: g_audio is overwritten every frame, so fold it here.
     * STATEHASH alone (fb+wram+cart) cannot detect an audio divergence. */
    uint64_t ah = fnv1a(g_audio, sizeof(g_audio));
    audio_hash = (audio_hash ^ ah) * 1099511628211ULL;

    if ((frame + 1) % RIG_WINDOW == 0) {
      uint64_t emu_i = win_emu * ipt_x1000 / 1000 / RIG_WINDOW;
      uint64_t apu_i = win_apu * ipt_x1000 / 1000 / RIG_WINDOW;
      int lit = 0;
      const uint16_t *lit_fb =
#ifdef RIG_DEVICE_VIDEO
        g_device_frame;
#else
        g_fb;
#endif
      for (int q = 0; q < 320 * 240; q++) if (lit_fb[q]) lit++;
      printf("w%05d emu=%lu apu=%lu insn/frame fb=%08lx audio=%08lx lit=%d\n",
             frame + 1, (unsigned long)emu_i, (unsigned long)apu_i,
             (unsigned long)(uint32_t)h, (unsigned long)(uint32_t)ah, lit);
      tot_emu += win_emu; tot_apu += win_apu;
      win_emu = win_apu = 0;
    }
  }
  uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
  if (frames == 0) frames = 1;
  uint64_t sh = fnv1a(g_wram, sizeof(g_wram)) ^ fnv1a(snes->cart->ram, snes->cart->ramSize);
#ifdef RIG_COST_PROF
  printf("[snes-qemu] done %d frames STATEHASH=%08lx AUDIOHASH=%08lx COREHASH=%08lx avg emu=%lu apu=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(uint32_t)audio_hash,
         (unsigned long)(uint32_t)sh,
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));
  printf("[cost] cpu=%lu spc700=%lu dsp=%lu present=%lu dsp_samples=%lu "
         "active_voices_x1000=%lu echo_voices_x1000=%lu echo_write_frames=%lu insn/frame\n",
         (unsigned long)(g_cpu_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_spc_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_present_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_calls / RIG_FRAMES),
         (unsigned long)(g_active_voice_sum * 1000 / RIG_FRAMES),
         (unsigned long)(g_echo_voice_sum * 1000 / RIG_FRAMES),
         (unsigned long)g_echo_write_frames);
#ifdef SNES_SPIN_SKIP
  { double n = (double)(g_spin.ops_real + g_spin.ops_virtual);
    printf("[spin] real=%llu virt=%llu skipped=%.4f%% gate=%d\n",
           (unsigned long long)g_spin.ops_real,
           (unsigned long long)g_spin.ops_virtual,
           n ? 100.0 * g_spin.ops_virtual / n : 0.0, (int)g_spin.gate_on); }
#endif
#ifdef RIG_PPU_DEEP
  printf("[ppu-deep] bg1=%lu bg2=%lu bg3=%lu sprite_eval=%lu sprite_draw=%lu "
         "clear=%lu palette=%lu fast=%lu math=%lu linecopy=%lu "
         "fast_pixels=%lu math_pixels=%lu insn/frame\n",
         (unsigned long)(g_ppu_bg_ticks[0] * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_bg_ticks[1] * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_bg_ticks[2] * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_sprite_eval_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_sprite_draw_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_clear_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_palette_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_fast_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_math_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_line_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_ppu_fast_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_pixels / RIG_FRAMES));
#endif
#else
  printf("[snes-qemu] done %d frames STATEHASH=%08lx AUDIOHASH=%08lx avg emu=%lu apu=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(uint32_t)audio_hash,
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));
#endif
  return 0;
}
