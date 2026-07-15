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

/* ---- purity counters (hooked cpu copy calls these) ---- */
uint64_t g_write_seq;
static uint64_t g_io_seq;
void snes_spin_read(Cpu *cpu, uint32_t adr) {
  uint32_t pcb = ((uint32_t)cpu->k << 16) | cpu->pc;
  if (adr - (pcb - 6) <= 12) return;
  uint8_t bank = adr >> 16;
  uint16_t off = (uint16_t)adr;
  bool wram = (bank == 0x7e || bank == 0x7f) ||
              (off < 0x2000 && (bank < 0x40 || (bank >= 0x80 && bank < 0xc0)));
  if (wram) return;
  bool rom = (off >= 0x8000) || (bank >= 0x40 && bank < 0x7e) || (bank >= 0xc0);
  if (rom) return;
  g_io_seq++;
}

/* ---- spin skip machinery (verbatim from skip_harness.c) ---- */
static uint64_t g_ops_real, g_ops_virtual;

#define PMAX 8
static struct { uint32_t pc[PMAX]; uint8_t charge[PMAX]; int len, idx; bool on; } sp;

#define LR 16
static struct { uint32_t pc; uint8_t charge; uint64_t w, io, r1, r2; } lr[LR];
static int lr_h, lr_n;

static void spin_note(uint32_t pc24, uint8_t charge, int dispatched,
                      uint64_t r1, uint64_t r2) {
  if (sp.on) {
    if (dispatched || pc24 != sp.pc[sp.idx]) sp.on = false;
    else sp.idx = (sp.idx + 1) % sp.len;
  }
  lr[lr_h].pc = pc24; lr[lr_h].charge = charge;
  lr[lr_h].w = g_write_seq; lr[lr_h].io = g_io_seq;
  lr[lr_h].r1 = r1; lr[lr_h].r2 = r2;
  lr_h = (lr_h + 1) % LR; if (lr_n < LR) lr_n++;
  if (sp.on || dispatched) return;

  for (int d = 1; d <= PMAX && 2 * d + 1 <= lr_n; d++) {
    int j = (lr_h - 1 - d + LR) % LR;
    if (lr[j].pc != pc24) continue;
    if (lr[j].r1 != r1 || lr[j].r2 != r2) return;
    int oldest = (lr_h - 1 - 2 * d + LR) % LR;
    if (lr[oldest].w != g_write_seq || lr[oldest].io != g_io_seq) return;
    if (lr[oldest].pc != pc24) return;
    if (lr[oldest].r1 != r1 || lr[oldest].r2 != r2) return;
    for (int q = 1; q < d; q++) {
      int a = (lr_h - 1 - q + LR) % LR, b = (lr_h - 1 - q - d + LR) % LR;
      if (lr[a].pc != lr[b].pc || lr[a].charge != lr[b].charge) return;
    }
    for (int q = 0; q < d; q++) {
      int a = (lr_h - d + q + LR) % LR;
      sp.pc[q] = lr[a].pc; sp.charge[q] = lr[a].charge;
    }
    sp.len = d; sp.idx = 0; sp.on = true;
    return;
  }
}

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
  uint64_t r1 = (uint64_t)cpu->a | ((uint64_t)cpu->x << 16) |
                ((uint64_t)cpu->y << 32) | ((uint64_t)cpu->sp << 48);
  uint64_t r2 = (uint64_t)cpu->dp | ((uint64_t)cpu->k << 16) |
                ((uint64_t)cpu->db << 24) | ((uint64_t)cpu_getFlags(cpu) << 32) |
                ((uint64_t)cpu->e << 40);
  snes->cpuMemOps = 0;
  int cycles = cpu_runOpcode(cpu);
  snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  g_ops_real++;
  spin_note(pc24, (uint8_t)snes->cpuCyclesLeft, disp, r1, r2);
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
      if (sp.on &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !snes->hIrqEnabled &&
          !(snes->vIrqEnabled && snes->vPos == snes->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == sp.pc[sp.idx]) {
        snes->cpuCyclesLeft += sp.charge[sp.idx];
        sp.idx = (sp.idx + 1) % sp.len;
        cpu->k  = (uint8_t)(sp.pc[sp.idx] >> 16);
        cpu->pc = (uint16_t)sp.pc[sp.idx];
        g_ops_virtual++;
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
  if (!snes_loadRom(snes, rom, (int)rom_len)) { printf("unsupported ROM\n"); return 1; }
  free(snes->cart->rom);
  snes->cart->rom = rom;
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
  { double tot = (double)(g_ops_real + g_ops_virtual);
    printf("[spin] real=%llu virt=%llu skipped=%.4f%%\n",
           (unsigned long long)g_ops_real, (unsigned long long)g_ops_virtual,
           tot > 0 ? 100.0 * g_ops_virtual / tot : 0.0); }
#ifdef RC_STATS
  { extern uint64_t g_rc_native, g_rc_interp;
    double tot = (double)(g_rc_native + g_rc_interp);
    printf("[rc] native=%llu interp=%llu coverage=%.4f%%\n",
           (unsigned long long)g_rc_native, (unsigned long long)g_rc_interp,
           tot > 0 ? 100.0 * g_rc_native / tot : 0.0); }
#endif
  return 0;
}
