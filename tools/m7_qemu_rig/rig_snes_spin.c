/* Generic SNES core on the M7 QEMU rig + the NMI-wait SPIN SKIP
 * (tools/snes_spin/skip_harness.c, host-gated bit-identical) ported into the
 * rig's event loop. Two builds from this one file:
 *   - spin-only:        interpreter cpu.c (sed-hooked purity counters)
 *   - spin+translator:  rc_core_rig.c hybrid (-DRC_STATS adds the [rc] print)
 *
 * The skip machinery is byte-for-byte skip_harness.c's: learn a loop after two
 * consecutive register-identical, write-free, IO-free iterations; then inside a
 * run_dots span replay the recorded ccl-charge pattern without running the CPU
 * — same chunk sizes, same FMA sequence — and drop back to real execution on
 * any pending interrupt, DMA, armed H-IRQ, V-IRQ line match, or PC mismatch.
 * Gate: STATEHASH must equal the stock hard-float run (92f52014 Zelda@1500tap).
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
#include "src/snes/spin_skip.h"   /* the ONE learner (core code, device-identical) */

#ifndef RIG_FRAMES
#define RIG_FRAMES 1200
#endif
#ifndef RIG_WINDOW
#define RIG_WINDOW 200
#endif

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);

void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

extern unsigned char _binary_rom_smc_start[];
extern unsigned char _binary_rom_smc_end[];

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

extern bool g_ppu_skip_render;
static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[16000 / 60];
static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* Learner + purity hooks live in src/snes/spin_skip.c (cpu.c calls them under
 * SNES_SPIN_SKIP) — this rig compiles the exact core the device compiles. */

/* ---- event loop (rig layout; run_dots grows the replay branch) ---- */
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

static int run_one_opcode(Snes *snes) {
  Cpu *cpu = snes->cpu;
  uint32_t pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
  int disp = (cpu->nmiWanted || (cpu->irqWanted && !cpu->i) || cpu->waiting) && !cpu->stopped;
  snes->cpuMemOps = 0;
  int cycles = cpu_runOpcode(cpu);
  snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  g_spin.ops_real++;
  spin_note(cpu, pc24, (uint8_t)snes->cpuCyclesLeft, disp);
  return cycles;
}

static void cpu_tick(Snes *snes) {
  if (dma_cycle(snes->dma)) return;
  if (snes->cpuCyclesLeft == 0) run_one_opcode(snes);
  snes->cpuCyclesLeft -= 2;
}

static void run_dots(Snes *snes, int dots) {
  Cpu *cpu = snes->cpu;
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma);
      snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (snes->cpuCyclesLeft == 0) {
      if (g_spin.on &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !snes->hIrqEnabled &&
          !(snes->vIrqEnabled && snes->vPos == snes->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == g_spin.pc[g_spin.idx]) {
        snes->cpuCyclesLeft += g_spin.charge[g_spin.idx];
        g_spin.idx = (g_spin.idx + 1) % g_spin.len;
        cpu->k  = (uint8_t)(g_spin.pc[g_spin.idx] >> 16);
        cpu->pc = (uint16_t)g_spin.pc[g_spin.idx];
        g_spin.ops_virtual++;
      } else {
        apply_irq_match(snes);
        run_one_opcode(snes);
        started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
      }
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
  spin_frame_tick();   /* device-identical auto-gate */
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

  *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
  __asm__ volatile("dsb; isb");

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("[snes-qemu] boot (spin-skip)\n");
  rig_timer_init();
  uint32_t cal_ticks = rig_calibrate(1000000);
  uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal_ticks ? cal_ticks : 1));
  printf("[snes-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
         (unsigned long)cal_ticks,
         (unsigned long)(ipt_x1000 / 1000), (unsigned long)(ipt_x1000 % 1000));

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  spin_whitelist_set(rom, rom_len);
  spin_reset();
  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\n"); return 1; }
#if !defined(GNW_SNES_CORE)
  /* Host builds: the loader malloc'd a pow2 copy; reuse the linked-in image.
   * GNW_SNES_CORE builds: cart_load already points at `rom` IN PLACE (zero-copy)
   * — freeing it here would free the ROM itself. */
  free(snes->cart->rom);
  snes->cart->rom = rom;
#endif
  printf("[snes-qemu] rom len=%lu frames=%d\n", (unsigned long)rom_len, RIG_FRAMES);

  uint64_t run_hash = 1469598103934665603ULL;
  uint64_t win_emu = 0, win_apu = 0, tot_emu = 0, tot_apu = 0;

  for (int frame = 0; frame < RIG_FRAMES; frame++) {
#ifdef RIG_INPUT_TAP
    snes->input1->currentState = (frame >= 40 && (frame % 24) < 6) ? 0x0008 : 0;
#else
    snes->input1->currentState = 0;
#endif
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);

#ifdef RIG_FRAMESKIP
    g_ppu_skip_render = true;
#endif
    uint32_t t0 = rig_timer_now();
    run_frame_events(snes);
    uint32_t t1 = rig_timer_now();
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
    }
    uint32_t t2 = rig_timer_now();
    win_emu += (uint32_t)(t1 - t0);
    win_apu += (uint32_t)(t2 - t1);

    uint64_t h = fnv1a(g_fb, sizeof(g_fb));
    run_hash = (run_hash ^ h) * 1099511628211ULL;

    if ((frame + 1) % RIG_WINDOW == 0) {
      uint64_t emu_i = win_emu * ipt_x1000 / 1000 / RIG_WINDOW;
      uint64_t apu_i = win_apu * ipt_x1000 / 1000 / RIG_WINDOW;
      int lit = 0;
      for (int q = 0; q < 320 * 240; q++) if (g_fb[q]) lit++;
      printf("w%05d emu=%lu apu=%lu insn/frame fb=%08lx lit=%d\n",
             frame + 1, (unsigned long)emu_i, (unsigned long)apu_i,
             (unsigned long)(uint32_t)h, lit);
      tot_emu += win_emu; tot_apu += win_apu;
      win_emu = win_apu = 0;
    }
  }
  uint64_t frames = (RIG_FRAMES / RIG_WINDOW) * RIG_WINDOW;
  if (frames == 0) frames = 1;
  uint64_t sh = fnv1a(g_wram, sizeof(g_wram)) ^ fnv1a(snes->cart->ram, snes->cart->ramSize);
  printf("[snes-qemu] done %d frames STATEHASH=%08lx avg emu=%lu apu=%lu insn/frame\n",
         RIG_FRAMES, (unsigned long)(uint32_t)(run_hash ^ sh),
         (unsigned long)(tot_emu * ipt_x1000 / 1000 / frames),
         (unsigned long)(tot_apu * ipt_x1000 / 1000 / frames));
  { double tot = (double)(g_spin.ops_real + g_spin.ops_virtual);
    printf("[spin] real=%llu virt=%llu skipped=%.4f%% gate=%d\n",
           (unsigned long long)g_spin.ops_real, (unsigned long long)g_spin.ops_virtual,
           tot > 0 ? 100.0 * g_spin.ops_virtual / tot : 0.0, (int)g_spin.gate_on); }
#ifdef RC_STATS
  { extern uint64_t g_rc_native, g_rc_interp;
    double tot = (double)(g_rc_native + g_rc_interp);
    printf("[rc] native=%llu interp=%llu coverage=%.4f%%\n",
           (unsigned long long)g_rc_native, (unsigned long long)g_rc_interp,
           tot > 0 ? 100.0 * g_rc_native / tot : 0.0); }
#endif
  return 0;
}
