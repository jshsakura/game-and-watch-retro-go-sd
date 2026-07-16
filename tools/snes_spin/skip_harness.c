/* NMI-wait spin skip — exact-replay prototype (SNES_SKIP=1 enables).
 *
 * The spin probe measured Zelda 81% / SMW 76% of gameplay opcodes inside pure
 * WRAM/DP-flag wait loops (`spin: LDA $12 / BEQ spin`). Those iterations are
 * semantic no-ops: registers bit-identical each pass, no writes, no IO reads —
 * only the NMI handler can change the polled byte, and no handler can run inside
 * a run_dots span (events fire only at span boundaries). So inside a span the
 * loop provably cannot exit, and each iteration can be replayed without the
 * interpreter: charge the recorded cycle pattern, advance pc along the recorded
 * opcode ring, and let the SAME bulk-consume code chunk the dots — identical
 * hPos steps, identical apuCatchupCycles FMA sequence, identical cpuCyclesLeft
 * arithmetic. Bit-identical state, minus the interpreter work.
 *
 * Learning: ring of recent (pc24, ccl-charge, write_seq, io_seq) opcode calls.
 * A pattern is adopted after two consecutive identical iterations (period <= 8)
 * with write_seq/io_seq frozen across both. Any real call out of pattern, any
 * pending interrupt, DMA, armed H-IRQ, or a V-IRQ matching this line drops back
 * to the interpreter (and relearns in 2 iterations).
 *
 * Gate: SNES_SKIP=0 vs =1 must print identical state and audio hashes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"
#include "src/snes/spin_skip.h"   /* the ONE learner: same code the device runs */

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);

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

static Snes *g_the_snes;
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[16000 / 60];

static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* The purity hooks, the learning ring and spin_note() live in
 * src/snes/spin_skip.c now (cpu.c calls the hooks itself under SNES_SPIN_SKIP) —
 * this harness compiles the exact files the device compiles and only keeps the
 * event loop + hashing around them. */
static int g_skip_enabled;

/* ---- event loop (snes_main.c layout; run_dots grows the replay branch) ---- */
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

static int run_one_opcode(Snes *snes) {  /* shared: real call + note */
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
  g_spin.ops_real++;
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
      /* ---- replay branch: virtual no-op iteration, no interpreter ---- */
      if (g_spin.on &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !snes->hIrqEnabled &&
          !(snes->vIrqEnabled && snes->vPos == snes->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == g_spin.pc[g_spin.idx]) {
        snes->cpuCyclesLeft += g_spin.charge[g_spin.idx];
        g_spin.idx = (g_spin.idx + 1) % g_spin.len;
        cpu->k  = (uint8_t)(g_spin.pc[g_spin.idx] >> 16);   /* pc parks at next opcode, */
        cpu->pc = (uint16_t)g_spin.pc[g_spin.idx];          /* exactly as after a real call */
        g_spin.ops_virtual++;
        /* fall through to the shared bulk-consume: same chunks, same FMAs */
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
  spin_frame_tick();   /* the device runs the auto-gate; so does the gate harness */
}

static uint64_t hash_state(Snes *snes) {
  uint64_t h = 1469598103934665603ULL;
  #define HASH(p, n) do { const uint8_t *b_ = (const uint8_t *)(p); \
    for (size_t i_ = 0; i_ < (size_t)(n); i_++) { h ^= b_[i_]; h *= 1099511628211ULL; } } while (0)
  HASH(g_fb, sizeof(g_fb));
  HASH(g_wram, sizeof(g_wram));
  HASH(snes->cart->ram, snes->cart->ramSize);
  return h;
}

int main(int argc, char **argv) {
  if (argc < 2) { printf("usage: skip_harness <rom> [frames]\n"); return 1; }
  int frames = argc > 2 ? atoi(argv[2]) : 1500;
  const char *sk = getenv("SNES_SKIP");
  g_skip_enabled = sk ? atoi(sk) : 0;
  spin_reset();
  g_spin.gate_on = g_skip_enabled != 0;   /* skip=0: learner parked forever = pure interpreter */

  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("no rom: %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0;
  fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n);
  if (fread(rom, 1, n, f) != (size_t)n) return 1;
  fclose(f);

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("unsupported ROM\n"); return 1; }

  uint64_t g_audiohash = 1469598103934665603ULL;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < frames; i++) {
    snes->input1->currentState = (i >= 40 && (i % 24) < 6) ? 0x0008 : 0;
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    run_frame_events(snes);
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
      const uint8_t *ab = (const uint8_t *)g_audio;
      for (size_t q = 0; q < sizeof(g_audio); q++) { g_audiohash ^= ab[q]; g_audiohash *= 1099511628211ULL; }
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);

  double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames;
  int lit = 0;
  for (int i = 0; i < 320 * 240; i++) if (g_fb[i]) lit++;
  printf("skip=%d %7.3f ms/frame  state=%016llx  audio=%016llx  lit=%d  real=%llu virt=%llu (%.1f%% skipped)%s\n",
         g_skip_enabled, ms,
         (unsigned long long)hash_state(snes), (unsigned long long)g_audiohash, lit,
         (unsigned long long)g_spin.ops_real, (unsigned long long)g_spin.ops_virtual,
         (g_spin.ops_real + g_spin.ops_virtual)
             ? 100.0 * g_spin.ops_virtual / (g_spin.ops_real + g_spin.ops_virtual) : 0.0,
         (g_skip_enabled && !g_spin.gate_on) ? "  [auto-gate PARKED]" : "");
  return 0;
}
