/* Generalized 65816 idle-spin probe (NMI-wait / flag-poll / WAI).
 *
 * The APU-port probe (tools/snes_idle) answered one narrow question: gameplay
 * APU-port spins = 0%. This probe answers the general one: how much of the
 * interpreter's work is ANY do-nothing wait — the classic RPG main loop
 * `spin: LDA $12 / BEQ spin` polling a WRAM flag that only the NMI handler
 * changes, IO polls ($4212 HVBJOY), and WAI.
 *
 * Detector (exact, address-agnostic): an opcode belongs to a spin iteration if
 * the PC recurs with period <= LOOPMAX opcodes and, between the two visits,
 *   - every CPU register and flag is bit-identical (packed snapshot), and
 *   - the CPU performed no memory write (g_write_seq unchanged).
 * Such an iteration is a semantic no-op: only an external event (NMI/IRQ/DMA)
 * can ever break it. Classified by what it reads:
 *   pure spin  - data reads touch only WRAM/DP/ROM  -> span-skippable exactly
 *                (nothing can change those during a run_dots span)
 *   io spin    - data reads touch $2000-$7FFF IO    -> NOT span-skippable
 *                ($4212 flips with hPos; APU ports move the SPC on catchup)
 * WAI is counted separately from the instrumented cpu_runOpcode waiting path:
 * each tick = one call consuming 6 dots with no opcode executed.
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

/* ---- event loop, verbatim from tools/snes_harness/snes_main.c ---- */
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

/* ------------------------------- spin detector ------------------------------ */
uint32_t *g_pchist;                   /* cpu.c's histogram hook needs the symbol; stays NULL */
uint64_t g_write_seq;                 /* bumped by instrumented cpu_write */
uint64_t g_wai_ticks;                 /* bumped by instrumented waiting path */
static uint64_t g_io_seq;             /* data reads that touched IO */
static uint32_t g_last_data_adr;      /* last non-fetch data read address */

uint64_t g_ops_total, g_ops_spin_pure, g_ops_spin_io;

/* Called from instrumented cpu_read on EVERY cpu bus read. Fetch/operand reads
 * sit within a few bytes of PC; anything else is a data read. */
void snes_spin_read(Cpu *cpu, uint32_t adr) {
  uint32_t pcb = ((uint32_t)cpu->k << 16) | cpu->pc;
  if (adr - (pcb - 6) <= 12) return;            /* fetch/operand vicinity */
  g_last_data_adr = adr;
  uint8_t bank = adr >> 16;
  uint16_t off = (uint16_t)adr;
  bool wram = (bank == 0x7e || bank == 0x7f) ||
              (off < 0x2000 && (bank < 0x40 || (bank >= 0x80 && bank < 0xc0)));
  if (wram) return;
  bool rom = (off >= 0x8000) || (bank >= 0x40 && bank < 0x7e) || (bank >= 0xc0);
  if (rom) return;
  g_io_seq++;                                    /* $2000-$7FFF style IO */
}

#define LOOPMAX 16
typedef struct { uint32_t pc; uint64_t r1, r2, wseq, ioseq; } RingEnt;
static RingEnt g_ring[LOOPMAX];
static int g_head, g_prim;

/* spin-site table */
#define NSITE 4096
typedef struct { uint32_t pc; uint64_t iters; uint8_t period, io; uint32_t adr; } Site;
static Site g_sites[NSITE];
static void site_bump(uint32_t pc, int period, int io, uint32_t adr) {
  uint32_t h = (pc * 2654435761u) & (NSITE - 1);
  for (int i = 0; i < 64; i++, h = (h + 1) & (NSITE - 1)) {
    if (g_sites[h].pc == pc || g_sites[h].pc == 0) {
      g_sites[h].pc = pc; g_sites[h].iters++;
      g_sites[h].period = (uint8_t)period; g_sites[h].io = (uint8_t)io;
      g_sites[h].adr = adr;
      return;
    }
  }
}

void snes_spin_op(Cpu *cpu) {
  uint32_t pc24 = (((uint32_t)cpu->k << 16) | (uint16_t)(cpu->pc - 1)) & 0xffffff;
  uint64_t r1 = (uint64_t)cpu->a | ((uint64_t)cpu->x << 16) |
                ((uint64_t)cpu->y << 32) | ((uint64_t)cpu->sp << 48);
  uint64_t r2 = (uint64_t)cpu->dp | ((uint64_t)cpu->k << 16) |
                ((uint64_t)cpu->db << 24) | ((uint64_t)cpu_getFlags(cpu) << 32) |
                ((uint64_t)cpu->e << 40);
  g_ops_total++;

  if (g_prim >= LOOPMAX) {
    for (int d = 1; d <= 8; d++) {              /* period <= 8 opcodes */
      RingEnt *p = &g_ring[(g_head - d + LOOPMAX) % LOOPMAX];
      if (p->pc != pc24) continue;
      if (p->r1 == r1 && p->r2 == r2 && p->wseq == g_write_seq) {
        /* count THIS opcode only (+1): each of the loop's d opcodes fires its own
         * detection once per iteration, so the sum is exact — adding d here would
         * overcount by a factor of d (the first probe build did; 162% told on it) */
        int io = (p->ioseq != g_io_seq);
        if (io) g_ops_spin_io += 1; else g_ops_spin_pure += 1;
        site_bump(pc24, d, io, g_last_data_adr);
      }
      break;
    }
  }
  RingEnt *e = &g_ring[g_head];
  e->pc = pc24; e->r1 = r1; e->r2 = r2; e->wseq = g_write_seq; e->ioseq = g_io_seq;
  g_head = (g_head + 1) % LOOPMAX;
  if (g_prim < LOOPMAX) g_prim++;
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
  if (argc < 2) { printf("usage: spin_probe <rom> [frames]\n"); return 1; }
  int frames = argc > 2 ? atoi(argv[2]) : 2000;

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

  uint64_t pf_ops = 0, pf_pure = 0, pf_io = 0, pf_wai = 0;
  uint64_t gp_ops = 0, gp_pure = 0, gp_io = 0, gp_wai = 0;
  int gp_frames = 0; const int WARMUP = 200;
  uint64_t g_audiohash = 1469598103934665603ULL;

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
    uint64_t dops = g_ops_total - pf_ops, dpure = g_ops_spin_pure - pf_pure,
             dio = g_ops_spin_io - pf_io, dwai = g_wai_ticks - pf_wai;
    pf_ops = g_ops_total; pf_pure = g_ops_spin_pure; pf_io = g_ops_spin_io; pf_wai = g_wai_ticks;
    if (i >= WARMUP) { gp_ops += dops; gp_pure += dpure; gp_io += dio; gp_wai += dwai; gp_frames++; }
    if ((i + 1) % 200 == 0) {
      int L = 0; for (int q = 0; q < 320 * 240; q++) if (g_fb[q]) L++;
      fprintf(stderr, "w%5d ops/f=%llu pure=%.1f%% io=%.1f%% waiTicks/f=%llu lit=%d\n",
              i + 1, (unsigned long long)dops,
              dops ? 100.0 * dpure / dops : 0.0, dops ? 100.0 * dio / dops : 0.0,
              (unsigned long long)dwai, L);
    }
  }

  int lit = 0;
  for (int i = 0; i < 320 * 240; i++) if (g_fb[i]) lit++;
  printf("state=%016llx audio=%016llx lit=%d\n",
         (unsigned long long)hash_state(snes), (unsigned long long)g_audiohash, lit);
  double ofr = gp_frames ? (double)gp_ops / gp_frames : 0;
  /* WAI: each tick = one waiting cpu_runOpcode call = 6 dots. 357368 dots/frame. */
  double wai_dots_pct = gp_frames ? 100.0 * (gp_wai * 6.0 / gp_frames) / 357368.0 : 0;
  printf("[spin] gameplay frames=%d ops/frame=%.0f  PURE-spin=%.2f%%  IO-spin=%.2f%%  WAIticks/frame=%.0f (%.1f%% of frame dots)\n",
         gp_frames, ofr,
         gp_ops ? 100.0 * gp_pure / gp_ops : 0.0,
         gp_ops ? 100.0 * gp_io / gp_ops : 0.0,
         gp_frames ? (double)gp_wai / gp_frames : 0.0, wai_dots_pct);
  /* top sites */
  for (int pass = 0; pass < 12; pass++) {
    uint64_t best = 0; int bi = -1;
    for (int i = 0; i < NSITE; i++)
      if (g_sites[i].pc && g_sites[i].iters > best) { best = g_sites[i].iters; bi = i; }
    if (bi < 0) break;
    printf("[site] $%02x:%04x period=%d %s polls=$%06x iters=%llu\n",
           g_sites[bi].pc >> 16, g_sites[bi].pc & 0xffff, g_sites[bi].period,
           g_sites[bi].io ? "IO  " : "PURE", g_sites[bi].adr,
           (unsigned long long)g_sites[bi].iters);
    g_sites[bi].pc = 0;   /* consumed */
  }
  return 0;
}
