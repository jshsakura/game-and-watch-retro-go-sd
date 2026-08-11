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

#ifdef RIG_FRAME_DIST
/* Per-frame EMU instruction distribution.  Records raw timer deltas per frame,
 * then sorts at end to report percentiles.  Lets us find the heavy-frame tail
 * (p99) that window averages hide. */
static uint32_t g_frame_emu_ticks[RIG_FRAMES];
static uint32_t g_frame_apu_ticks[RIG_FRAMES];
static int cmp_u32(const void *a, const void *b) {
  uint32_t va = *(const uint32_t *)a, vb = *(const uint32_t *)b;
  return (va > vb) - (va < vb);
}
static void rig_print_percentiles(const char *label, uint32_t *arr, int n, uint32_t ipt_x1000) {
  /* Sort a copy so the original temporal order survives if needed later. */
  static uint32_t sorted[RIG_FRAMES];
  int copy_n = n < RIG_FRAMES ? n : RIG_FRAMES;
  for (int i = 0; i < copy_n; i++) sorted[i] = arr[i];
  qsort(sorted, copy_n, sizeof(uint32_t), cmp_u32);
  uint32_t p_idx[] = {0, 1, 5, 10, 25, 50, 75, 90, 95, 99};
  const char *p_name[] = {"min","p1","p5","p10","p25","p50","p75","p90","p95","p99"};
  printf("[dist] %s (n=%d):", label, copy_n);
  for (int i = 0; i < 10; i++) {
    int idx = (int)((uint32_t)p_idx[i] * copy_n / 100);
    if (idx >= copy_n) idx = copy_n - 1;
    uint64_t insn = (uint64_t)sorted[idx] * ipt_x1000 / 1000;
    printf(" %s=%lu", p_name[i], (unsigned long)insn);
  }
  uint64_t max_insn = (uint64_t)sorted[copy_n - 1] * ipt_x1000 / 1000;
  printf(" max=%lu", (unsigned long)max_insn);
  uint64_t p50_i = (uint64_t)sorted[copy_n/2] * ipt_x1000 / 1000;
  uint64_t p90_i = (uint64_t)sorted[(int)(90*copy_n/100)] * ipt_x1000 / 1000;
  uint64_t p99_i = (uint64_t)sorted[(int)(99*copy_n/100)] * ipt_x1000 / 1000;
  printf(" | fps@312M: p50=%.1f p90=%.1f p99=%.1f max=%.1f\n",
         p50_i ? 312000000.0/p50_i : 0,
         p90_i ? 312000000.0/p90_i : 0,
         p99_i ? 312000000.0/p99_i : 0,
         max_insn ? 312000000.0/max_insn : 0);
}
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
uint64_t g_dsp_channel_ticks, g_dsp_mix_ticks, g_dsp_echo_ticks;
uint64_t g_dsp_noise_ticks, g_dsp_store_ticks;
static uint64_t g_present_ticks;
static uint64_t g_win_cpu_ticks;
#define PROFILE_CPU(expr) ({ \
  uint32_t _ct = rig_timer_now(); \
  int _cycles = (expr); \
  uint32_t _dt = (uint32_t)(rig_timer_now() - _ct); \
  g_cpu_ticks += _dt; g_win_cpu_ticks += _dt; \
  _cycles; \
})
#else
#define PROFILE_CPU(expr) (expr)
#endif
#ifdef RIG_CALL_PROFILE
uint64_t g_cpuRead_calls, g_cpuRead_slow, g_cpuRead_romhit, g_cpuRead_wram;
uint64_t g_cpuWrite_calls, g_cpuWrite_slow;
uint64_t g_dma_cycle_calls, g_dma_cycle_true;
uint64_t g_dma_doDma_calls, g_dma_doHdma_calls;
uint64_t g_win_cpuRead_calls, g_win_dma_cycle_calls;
uint64_t g_irq_calls, g_irq_skip, g_irq_match;
uint64_t g_opcode_calls;
#endif

#ifdef RIG_PPU_DEEP
uint64_t g_ppu_bg_ticks[3];
uint64_t g_ppu_sprite_eval_ticks, g_ppu_sprite_draw_ticks;
uint64_t g_ppu_clear_ticks, g_ppu_palette_ticks;
uint64_t g_ppu_fast_ticks, g_ppu_math_ticks, g_ppu_line_ticks;
uint64_t g_ppu_fast_pixels, g_ppu_math_pixels;
uint64_t g_ppu_math_applied_pixels, g_ppu_math_bypass_pixels;
uint64_t g_ppu_math_fixed_pixels, g_ppu_math_subscreen_pixels;
uint64_t g_ppu_math_add_pixels, g_ppu_math_subtract_pixels, g_ppu_math_half_pixels;
uint64_t g_ppu_math_rebuild_ticks, g_ppu_math_rebuild_calls;
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

#ifdef RIG_AUDIO_DUMP
/* Semihosted binary file write. stdout is SYS_WRITE0, which stops at the first
 * NUL and so cannot carry PCM; this opens a real file instead. Host side reads
 * it as raw little-endian int16 mono at 16 kHz, one frame of 266 after another. */
static uint32_t sh(uint32_t op, void *arg) {
  register uint32_t r0 __asm__("r0") = op;
  register void *r1 __asm__("r1") = arg;
  __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}
static int g_audio_fd = -1;
static void rig_audio_dump(const void *p, unsigned n) {
  if (g_audio_fd < 0) {
#ifndef RIG_AUDIO_PATH
#define RIG_AUDIO_PATH "/tmp/rig_audio.pcm"
#endif
    static const char path[] = RIG_AUDIO_PATH;
    uint32_t a[3] = { (uint32_t)(uintptr_t)path, 6 /* "wb" */, sizeof(path) - 1 };
    g_audio_fd = (int)sh(0x01, a);      /* SYS_OPEN */
    if (g_audio_fd <= 0) { g_audio_fd = -2; return; }
  }
  if (g_audio_fd < 0) return;
  uint32_t a[3] = { (uint32_t)g_audio_fd, (uint32_t)(uintptr_t)p, n };
  sh(0x05, a);                          /* SYS_WRITE */
}
#endif

#ifdef RIG_DEVICE_VIDEO
static uint16_t g_line[256];
/* The normal 224-line image starts at row 8.  Keep eight hidden tail rows so
 * an overscan frame (240 lines) can still render directly without writing
 * beyond the backing store; only the visible first 240 rows are presented. */
static uint16_t g_device_frame[320 * (240 + 8)];
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
#ifdef RIG_CALL_PROFILE
  g_irq_calls++;
#endif
  if (!(snes->hIrqEnabled || snes->vIrqEnabled)) {
#ifdef RIG_CALL_PROFILE
    g_irq_skip++;
#endif
    return;
  }
  if (snes->vIrqEnabled && snes->vPos != snes->vTimer) {
#ifdef RIG_CALL_PROFILE
    g_irq_skip++;
#endif
    return;
  }
  if (snes->hIrqEnabled && snes->hPos != snes->hTimer * 4) {
#ifdef RIG_CALL_PROFILE
    g_irq_skip++;
#endif
    return;
  }
#ifdef RIG_CALL_PROFILE
  g_irq_match++;
#endif
  snes->inIrq = true;
  snes->cpu->irqWanted = true;
}

#ifdef SNES_SPIN_SKIP
static int run_one_opcode(Snes *snes) {
  Cpu *cpu = snes->cpu;
  const bool learn = spin_engaged();   /* device-identical: main_snes.c */
  uint32_t pc24 = 0;
  int dispatch = 0;
  if (learn) {
    pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
    dispatch = (cpu->nmiWanted || (cpu->irqWanted && !cpu->i) || cpu->waiting) &&
               !cpu->stopped;
  }
  snes->cpuMemOps = 0;
#ifdef RIG_CALL_PROFILE
  g_opcode_calls++;
#endif
  int cycles = PROFILE_CPU(CPU_RUN_OPCODE(cpu));
  snes->cpuCyclesLeft += cycles * 6 + snes->cpuMemOps * 2;
  if (learn) spin_note_real(cpu, pc24, (uint8_t)snes->cpuCyclesLeft, dispatch);
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
#ifdef RIG_CALL_PROFILE
    g_opcode_calls++;
#endif
    int cycles = PROFILE_CPU(CPU_RUN_OPCODE(snes->cpu));
    snes->cpuCyclesLeft += cycles * 6 + snes->cpuMemOps * 2;
#endif
  }
  snes->cpuCyclesLeft -= 2;
}
static void run_dots(Snes *snes, int dots) {
  /* Hoist DMA-active check into a local. DMA is only active during HDMA
   * countdown (hPos 1024->1362) and VBlank bulk transfer. Between those
   * windows (2/3 of scanline segments), dma_active is false for the entire
   * call, so the per-opcode path avoids 2 memory loads + OR + branch.
   * Safety: dma_active can only go true via an opcode writing DMAEN ($420b),
   * which drains synchronously in snes_writeReg - so started_dma catches it.
   * hdmaTimer can only increase via dma_doHdma/dma_initHdma, called from
   * snes_handle_pos_stuff, OUTSIDE run_dots. */
   bool dma_active = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
  while (dots > 0) {
    if (dma_active) {
      dma_cycle(snes->dma);
      snes->apuDotsAccum += 2;
      snes->hPos += 2; dots -= 2;
      dma_active = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
      continue;
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
#ifdef RIG_CALL_PROFILE
      g_opcode_calls++;
#endif
      int cycles = PROFILE_CPU(CPU_RUN_OPCODE(snes->cpu));
      snes->cpuCyclesLeft += cycles * 6 + snes->cpuMemOps * 2;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
#endif
    }
    int step;
    if (snes->cpuCyclesLeft >= 2 && !started_dma) {
      step = snes->cpuCyclesLeft;
      if (step > dots) step = dots;
      snes->cpuCyclesLeft -= (uint8_t)step;
    } else {
      step = 2;
      snes->cpuCyclesLeft -= 2;
    }
    snes->apuDotsAccum += step;
    snes->hPos += step; dots -= step;
    dma_active = started_dma;
  }
}
static void run_frame_events(Snes *snes) {
  for (;;) {
    snes->apuDotsAccum += 2;
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
#ifdef RIG_INPUT_SMK
    uint16_t pad = 0;
    if (frame < 200) pad = 0;
    else if (frame < 210) pad = 0x0008;         /* Start at title */
    else if (frame < 350) pad = 0;
    else if (frame < 360) pad = 0x0100;         /* A: mode select */
    else if (frame < 450) pad = 0;
    else if (frame < 460) pad = 0x0100;         /* A: player count */
    else if (frame < 550) pad = 0;
    else if (frame < 560) pad = 0x0100;         /* A: character */
    else if (frame < 650) pad = 0;
    else if (frame < 660) pad = 0x0100;         /* A: cup */
    else if (frame < 750) pad = 0;
    else if (frame < 760) pad = 0x0100;         /* A: start race */
    else if (frame >= 900) {
      int gp = frame - 900;
      int phase = (gp / 90) % 4;
      int step = gp % 90;
      if (step < 60) {
        switch (phase) {
          case 0: pad = 0x0080; break;
          case 1: pad = 0x0040; break;
          case 2: pad = 0x0020; break;
          case 3: pad = 0x0010; break;
        }
      }
      if (gp % 37 < 4) pad |= 0x0100;
    }
    snes->input1->currentState = pad;
#else
    if (frame < 900) {
      snes->input1->currentState = (frame >= 40 && (frame % 24) < 6) ? 0x0008 : 0;
    } else {
      uint16_t pad = 0;
      int gp = frame - 900;
      int phase = (gp / 90) % 4;
      int step = gp % 90;
      if (step < 60) {
        switch (phase) {
          case 0: pad = 0x0080; break;
          case 1: pad = 0x0040; break;
          case 2: pad = 0x0020; break;
          case 3: pad = 0x0010; break;
        }
      }
      if (gp % 37 < 4) pad |= 0x0100;
      snes->input1->currentState = pad;
    }
#endif
#else
    snes->input1->currentState = 0;
#endif
#ifdef RIG_DEVICE_VIDEO
#ifdef RIG_DIRECT_VIDEO
    g_ppu_line_cb = NULL;
    PpuBeginDrawing(snes->ppu,
                    (uint8_t *)(g_device_frame + 8 * 320 + 32),
                    320 * 2, 0);
#else
    g_ppu_line_cb = &rig_blit_line;
    PpuBeginDrawing(snes->ppu, (uint8_t *)g_line, 0, 0);
#endif
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
#ifdef RIG_AUDIO_DUMP
    /* The frame's emulated samples, raw, in emitted order. This rig already
     * produces exactly what the device's snes_pcm_submit() hands the stretcher
     * -- it just hashed them and threw them away, which is why the stretcher's
     * output has only ever been judged by ear on hardware. Written out, the
     * same samples can be pushed through the real snes_audio_stretch.c on a
     * host (tools/snes_stretch_sim) and the result counted AND listened to. */
    rig_audio_dump(g_audio, sizeof(g_audio));
#endif
    uint32_t t2 = rig_timer_now();
    win_emu += (uint32_t)(t1 - t0);
    win_apu += (uint32_t)(t2 - ta);
#ifdef RIG_FRAME_DIST
    g_frame_emu_ticks[frame] = (uint32_t)(t1 - t0);
    g_frame_apu_ticks[frame] = (uint32_t)(t2 - ta);
#endif

#ifdef RIG_DEVICE_VIDEO
    /* The extra eight rows only make direct overscan writes memory-safe; they
     * are outside the LCD image and must not participate in the video gate. */
    uint64_t h = fnv1a(g_device_frame, sizeof(g_lcd_frame));
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
#ifdef RIG_COST_PROF
      printf("        cpu=%lu", (unsigned long)(g_win_cpu_ticks * ipt_x1000 / 1000 / RIG_WINDOW));
      g_win_cpu_ticks = 0;
#endif
#ifdef RIG_CALL_PROFILE
      printf(" rdcalls=%lu rdslow=%lu rdrom=%lu rdwram=%lu dmcyc=%lu dmactrue=%lu dodma=%lu dohdma=%lu irq=%lu skip=%lu match=%lu opc=%lu",
             (unsigned long)(g_win_cpuRead_calls / RIG_WINDOW),
             (unsigned long)(g_cpuRead_slow / RIG_WINDOW),  /* window-wide delta since last print */
             (unsigned long)(g_cpuRead_romhit / RIG_WINDOW),
             (unsigned long)(g_cpuRead_wram / RIG_WINDOW),
             (unsigned long)(g_win_dma_cycle_calls / RIG_WINDOW),
             (unsigned long)(g_dma_cycle_true / RIG_WINDOW),
             (unsigned long)(g_dma_doDma_calls / RIG_WINDOW),
             (unsigned long)(g_dma_doHdma_calls / RIG_WINDOW),
             (unsigned long)(g_irq_calls / RIG_WINDOW),
             (unsigned long)(g_irq_skip / RIG_WINDOW),
             (unsigned long)(g_irq_match / RIG_WINDOW),
             (unsigned long)(g_opcode_calls / RIG_WINDOW));
      g_cpuRead_slow = g_cpuRead_romhit = g_cpuRead_wram = 0;
      g_dma_cycle_true = g_dma_doDma_calls = g_dma_doHdma_calls = 0;
      g_win_cpuRead_calls = g_win_dma_cycle_calls = 0;
      g_irq_calls = g_irq_skip = g_irq_match = 0;
      g_opcode_calls = 0;
#endif
      printf("\n");
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
#ifdef RIG_DSP_DEEP
  printf("[dsp-deep] channels=%lu mix=%lu echo=%lu noise=%lu store=%lu insn/frame\n",
         (unsigned long)(g_dsp_channel_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_mix_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_echo_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_noise_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)(g_dsp_store_ticks * ipt_x1000 / 1000 / frames));
#endif
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
         "fast_pixels=%lu math_pixels=%lu applied=%lu bypass=%lu "
         "fixed=%lu subscreen=%lu add=%lu subtract=%lu half=%lu insn/frame\n",
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
         (unsigned long)(g_ppu_math_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_applied_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_bypass_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_fixed_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_subscreen_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_add_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_subtract_pixels / RIG_FRAMES),
         (unsigned long)(g_ppu_math_half_pixels / RIG_FRAMES));
  printf("[ppu-cache] rebuild=%lu calls=%lu insn/frame\n",
         (unsigned long)(g_ppu_math_rebuild_ticks * ipt_x1000 / 1000 / frames),
         (unsigned long)g_ppu_math_rebuild_calls);
#endif
#else
#if SNES_RENDER_CENSUS
  /* The rig cannot price a cache hit -- QEMU has none -- but it can COUNT one.
   * Whether consecutive tilemap entries name the same tile is a property of the
   * ROM's content, not of the silicon, so the tile memo's hit rate is knowable
   * here even though its value is not. */
  {
    extern uint32_t g_tile_same, g_tile_diff, g_bg_tile[2], g_render_lines;
    extern uint32_t g_sub_lines, g_bg_pass[2], g_sprite_slivers, g_sprite_lines;
    unsigned long tot = (unsigned long)g_tile_same + g_tile_diff;
    printf("[snes-qemu] tile memo: same=%lu diff=%lu hit=%lu%%  bg_tile=%lu/%lu lines=%lu\n",
           (unsigned long)g_tile_same, (unsigned long)g_tile_diff,
           tot ? 100UL * g_tile_same / tot : 0UL,
           (unsigned long)g_bg_tile[0], (unsigned long)g_bg_tile[1],
           (unsigned long)g_render_lines);
    printf("[snes-qemu] coverage: bg_pass=%lu/%lu sub_lines=%lu\n",
           (unsigned long)g_bg_pass[0], (unsigned long)g_bg_pass[1],
           (unsigned long)g_sub_lines);
    printf("[snes-qemu] coverage: sprite_slivers=%lu sprite_lines=%lu\n",
           (unsigned long)g_sprite_slivers, (unsigned long)g_sprite_lines);
    {
      extern uint32_t g_tile_full[2], g_tile_mixed[2], g_tile_flat[2], g_tile_opq_z[2];
      extern uint64_t g_tile_opaque_px[2];
      printf("[snes-qemu] opaque path taken: flat(main/sub)=%lu/%lu  ztest=%lu/%lu\n",
             (unsigned long)g_tile_flat[0], (unsigned long)g_tile_flat[1],
             (unsigned long)g_tile_opq_z[0], (unsigned long)g_tile_opq_z[1]);
      for (int sc = 0; sc < 2; sc++) {
        unsigned long f = g_tile_full[sc], m = g_tile_mixed[sc], d = f + m;
        if (!d) continue;
        printf("[snes-qemu] %s tiles decoded=%lu full-opaque=%lu (%lu%%) mixed=%lu"
               "  opaque px/tile=%.2f\n", sc ? "sub " : "main",
               d, f, 100UL * f / d, m, (double)g_tile_opaque_px[sc] / d);
      }
    }
    /* A hash gate proves nothing about code the run never reaches, and this run
     * reaches less than it looks like it does: at 400 frames A Link to the Past
     * decodes ZERO background tiles and renders ZERO subscreen lines, because it
     * is still on a black screen. Say so out loud rather than letting a green
     * hash stand in for coverage. */
    if (g_bg_tile[0] + g_bg_tile[1] == 0)
      printf("[snes-qemu] COVERAGE WARNING: no background tile was decoded -- "
             "this run does not gate the tile drawers. Use more frames.\n");
    if (g_sub_lines == 0)
      printf("[snes-qemu] COVERAGE WARNING: no subscreen line was rendered -- "
             "this run does not gate the colour-math path. Use more frames.\n");
  }
#endif
  printf("[snes-qemu] done %d frames STATEHASH=%08lx AUDIOHASH=%08lx avg emu=%lu apu=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(uint32_t)audio_hash,
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));
#if SNES_OP_CENSUS
  { extern uint32_t g_op_total, g_op_fallback, g_op_fbhist[256];
    printf("[snes-qemu] OPCEN total=%lu fallback=%lu (%.2f%%)\n",
           (unsigned long)g_op_total, (unsigned long)g_op_fallback,
           g_op_total ? 100.0 * g_op_fallback / g_op_total : 0.0);
    for (int pass = 0; pass < 12; pass++) {
      int best = -1; uint32_t bv = 0;
      for (int o = 0; o < 256; o++) if (g_op_fbhist[o] > bv) { bv = g_op_fbhist[o]; best = o; }
      if (best < 0 || bv == 0) break;
      printf("[snes-qemu] OPCEN  op %02x  %lu  (%.2f%% of all)\n", best,
             (unsigned long)bv, 100.0 * bv / (g_op_total ? g_op_total : 1));
      g_op_fbhist[best] = 0;
    } }
#endif
#if SNES_DSP_CENSUS
  { extern uint32_t g_dsp_ticks, g_dsp_idle, g_dsp_active, g_dsp_pm, g_dsp_brr;
    printf("[snes-qemu] DSPCEN ticks=%lu idle=%lu active=%lu pm=%lu brr=%lu\n",
           (unsigned long)g_dsp_ticks, (unsigned long)g_dsp_idle,
           (unsigned long)g_dsp_active, (unsigned long)g_dsp_pm,
           (unsigned long)g_dsp_brr); }
#endif
#endif
#ifdef SNES_LINE_REUSE_PROBE
  ppu_lineReuseProbeReport();
  ppu_lineProbeFieldReport();
#endif
#ifdef SNES_LINE_CACHE
  ppu_lineCacheReport();
#endif
#ifdef RIG_DSP_KEYON_PROBE
  extern void dsp_keyonProbeReport(void);
  dsp_keyonProbeReport();
#endif
#ifdef RIG_FRAME_DIST
  /* Split the distribution by phase: boot (0-299), transition (300-599),
   * early gameplay (600-899), gameplay (900-end).  Window averages hide the
   * heavy-frame tail; percentiles expose it. */
  printf("\n[dist] === per-frame distribution (insn/frame at ~%lu.%03lu insn/tick) ===\n",
         (unsigned long)(ipt_x1000/1000), (unsigned long)(ipt_x1000%1000));
  rig_print_percentiles("EMU all", g_frame_emu_ticks, RIG_FRAMES, ipt_x1000);
  rig_print_percentiles("EMU 0-299", g_frame_emu_ticks, 300, ipt_x1000);
  rig_print_percentiles("EMU 300-599", g_frame_emu_ticks + 300, 300, ipt_x1000);
  rig_print_percentiles("EMU 600-899", g_frame_emu_ticks + 600, 300, ipt_x1000);
  if (RIG_FRAMES > 900)
    rig_print_percentiles("EMU 900+", g_frame_emu_ticks + 900, RIG_FRAMES - 900, ipt_x1000);
  rig_print_percentiles("APU all", g_frame_apu_ticks, RIG_FRAMES, ipt_x1000);
  if (RIG_FRAMES > 900)
    rig_print_percentiles("APU 900+", g_frame_apu_ticks + 900, RIG_FRAMES - 900, ipt_x1000);
#endif
  return 0;
}
