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

/* ---- purity counters (instrumented cpu.c copy) ---- */
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

/* ---- spin skip machinery ---- */
static int g_skip_enabled;
static uint64_t g_ops_real, g_ops_virtual;

#define PMAX 8
static struct { uint32_t pc[PMAX]; uint8_t charge[PMAX]; int len, idx; bool on; } sp;

#define LR 16
static struct { uint32_t pc; uint8_t charge; uint64_t w, io, r1, r2; } lr[LR];
static int lr_h, lr_n;

/* Record one real opcode call (pre-call pc24, total ccl charge, pre-call regs).
 * Keeps the pattern index in sync with real execution; learns a new pattern when
 * two consecutive pure iterations match. `dispatched` = an interrupt entered
 * cpu_runOpcode before the opcode — the executed opcode is NOT at pc24, so the
 * pattern must drop.
 *
 * Register identity is REQUIRED, not optional: a delay loop (`dey / bne`) writes
 * nothing and reads nothing yet terminates on its own — without the regs check
 * it gets adopted and replayed forever (first build did exactly that and died in
 * cart_readLorom). Equal regs at the same PC + no writes + no IO reads = the
 * machine state truly recurred, so the loop provably cannot exit by itself. */
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
  if (sp.on || !g_skip_enabled || dispatched) return;

  for (int d = 1; d <= PMAX && 2 * d + 1 <= lr_n; d++) {
    int j = (lr_h - 1 - d + LR) % LR;
    if (lr[j].pc != pc24) continue;
    /* regs identical at this PC on both prior visits */
    if (lr[j].r1 != r1 || lr[j].r2 != r2) return;
    /* wseq/ioseq frozen across the last TWO iterations */
    int oldest = (lr_h - 1 - 2 * d + LR) % LR;
    if (lr[oldest].w != g_write_seq || lr[oldest].io != g_io_seq) return;
    if (lr[oldest].pc != pc24) return;
    if (lr[oldest].r1 != r1 || lr[oldest].r2 != r2) return;
    for (int q = 1; q < d; q++) {
      int a = (lr_h - 1 - q + LR) % LR, b = (lr_h - 1 - q - d + LR) % LR;
      if (lr[a].pc != lr[b].pc || lr[a].charge != lr[b].charge) return;
    }
    /* adopt: entries [lr_h-d .. lr_h-1] are one iteration ending at pc24;
     * the next opcode to execute is the one that followed the previous pc24 */
    for (int q = 0; q < d; q++) {
      int a = (lr_h - d + q + LR) % LR;
      sp.pc[q] = lr[a].pc; sp.charge[q] = lr[a].charge;
    }
    sp.len = d; sp.idx = 0; sp.on = true;
    return;
  }
}

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
      /* ---- replay branch: virtual no-op iteration, no interpreter ---- */
      if (sp.on && g_skip_enabled &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !snes->hIrqEnabled &&
          !(snes->vIrqEnabled && snes->vPos == snes->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == sp.pc[sp.idx]) {
        snes->cpuCyclesLeft += sp.charge[sp.idx];
        sp.idx = (sp.idx + 1) % sp.len;
        cpu->k  = (uint8_t)(sp.pc[sp.idx] >> 16);   /* pc parks at next opcode, */
        cpu->pc = (uint16_t)sp.pc[sp.idx];          /* exactly as after a real call */
        g_ops_virtual++;
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
  printf("skip=%d %7.3f ms/frame  state=%016llx  audio=%016llx  lit=%d  real=%llu virt=%llu (%.1f%% skipped)\n",
         g_skip_enabled, ms,
         (unsigned long long)hash_state(snes), (unsigned long long)g_audiohash, lit,
         (unsigned long long)g_ops_real, (unsigned long long)g_ops_virtual,
         (g_ops_real + g_ops_virtual) ? 100.0 * g_ops_virtual / (g_ops_real + g_ops_virtual) : 0.0);
  return 0;
}
