/* SNES "CPU dead-wait axis" survey: WAI + register-poll spin loops.
 *
 * Answers: what fraction of run_dots() dots does the CPU spend not doing real
 * work -- either parked in WAI (0xcb, snes->cpu->waiting) or busy-spinning on
 * a hardware-register poll ($4210/$4211/$4212 HV/NMI, or $2140-$2143 APU
 * handshake)? This is a dot/instruction-accounting measurement, NOT a device-
 * fps claim -- it ranks which dead-wait joint is worth a game-agnostic
 * fast-forward lever (see snes-generic-lever-map-0721 memory).
 *
 * Cloned from snes_survey.c (never edit external/sm -- this is the only place
 * instrumentation goes). The driver-signature scan is dropped; only the boot
 * + frame loop + dead-wait sampling survives.
 *
 * WAI is a clean flag (cpu->waiting). The three poll groups have no read hook
 * in external/sm (snes_readReg/apu inPorts reads are plain, unhooked --
 * confirmed by reading snes.c/apu.c; CpuOpcodeHook is BRK-only, cpu.c:795,
 * NOT a per-opcode trace hook, despite that being the original plan) so they
 * are detected structurally instead, entirely in code this file owns:
 *   1. Track the (bank,pc) of every real opcode dispatch (skipping WAI
 *      continuations, which never change pc) in a small ring.
 *   2. If the current (bank,pc) matches one seen `period` (<=POLL_RING)
 *      dispatches ago, and that same period repeats >=3 times in a row, the
 *      CPU is confirmed spinning in a `period`-instruction loop.
 *   3. Classify the loop ONCE (byte-scan its instruction window, fetched via
 *      the public snes_read() bus-read function, for absolute-addressing
 *      operand bytes matching $4210-12 or $2140-43) and cache the verdict by
 *      (bank, loop-start-pc) so repeat visits are free.
 *   4. Attribute that dispatch's dots to whichever group the loop classified
 *      as (HV/NMI poll, APU poll, or an unclassified "other tight loop").
 * This is an approximation by construction (loop confirmation needs a few
 * iterations to build confidence, so a loop's opening dots land in neither
 * bucket) -- consistent with the WAI survey's proxy/floor caveats.
 *
 * Headless limitation: without START, only the title/attract state runs, so
 * this measures a FLOOR, not real gameplay. See tools/snes_survey/README.md.
 *
 * Output (one line per ROM, tab-separated):
 *   <rom-basename>\tOK\t<lit>\tWAI_DOT=<pct>\tWAI_TICK=<pct>\tHV_DOT=<pct>\tAPU_DOT=<pct>\tOTHER_DOT=<pct>
 *   <rom-basename>\tLOAD_FAIL\t-\t-\t-\t-\t-\t-
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <libgen.h>

#include "src/snes/snes.h"
#include "src/snes/cart.h"
#include "src/snes/ppu.h"
#include "src/snes/apu.h"
#include "src/snes/cpu.h"
#include "src/snes/dma.h"
#include "src/snes/input.h"

bool snes_loadRom(Snes* snes, const uint8_t* data, int length);

/* firmware allocator shims */
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

static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---- dead-wait accounting ---------------------------------------------------
 * Gated: frames before FRAME_SKIP are boot/init noise, excluded from every
 * group's numerator and the shared denominator. */
#define FRAME_SKIP 120
static bool     g_wai_gate = false;
static uint64_t g_wai_dots = 0;
static uint64_t g_total_dots = 0;
static uint64_t g_wai_ticks = 0;
static uint64_t g_total_ticks = 0;

/* Poll-loop detector state (real dispatches only, i.e. !was_waiting). */
#define POLL_RING 12
#define POLL_CONFIRM_HITS 3
static uint32_t g_pc_ring[POLL_RING];   /* [0] = most recent */
static int      g_ring_len = 0;
static int      g_loop_period = 0;
static int      g_loop_hits = 0;
static int      g_loop_group = 0;       /* 0=building, 1=HV, 2=APU, 3=other */

#define CLASSIFY_CACHE_SIZE 256
static bool     g_cls_used[CLASSIFY_CACHE_SIZE];
static uint32_t g_cls_key[CLASSIFY_CACHE_SIZE];
static uint8_t  g_cls_group[CLASSIFY_CACHE_SIZE];

static uint64_t g_hv_dots = 0, g_apu_dots = 0, g_other_dots = 0;

/* Byte-scan a loop's instruction window for absolute-addressing operand bytes
 * matching $4210/$4211/$4212 (HV/NMI) or $2140-$2143 (APU handshake). Reads
 * only the CODE bytes at [min_pc,max_pc] via the public bus-read snes_read()
 * -- never the register addresses themselves (those only appear as literal
 * operand byte *values* inside the instruction stream, never dereferenced
 * here), so this has no hardware side effects (no RDNMI-clear, no APU port
 * consumption). Guards away from register/mirror space (banks < 0x40 or
 * 0x80-0xbf, offset 0x2000-0x7fff) defensively -- real code never executes
 * from there, but a corrupted PC should fall back to "other", not misread
 * a live register. */
static int classify_loop(Snes *snes, uint32_t bank, int min_pc, int max_pc) {
  bool low_bank = (bank < 0x40) || (bank >= 0x80 && bank < 0xc0);
  if (low_bank && min_pc >= 0x2000 && min_pc < 0x8000) return 3;
  int lo = min_pc, hi = max_pc + 4;
  if (hi - lo > 40) hi = lo + 40;
  uint8_t buf[44];
  int n = hi - lo + 1; if (n > 44) n = 44; if (n < 2) return 3;
  for (int i = 0; i < n; i++) buf[i] = snes_read(snes, (bank << 16) | ((lo + i) & 0xffff));
  for (int i = 0; i + 1 < n; i++)
    if (buf[i+1] == 0x42 && (buf[i] == 0x10 || buf[i] == 0x11 || buf[i] == 0x12)) return 1;
  for (int i = 0; i + 1 < n; i++)
    if (buf[i+1] == 0x21 && buf[i] >= 0x40 && buf[i] <= 0x43) return 2;
  return 3;
}

/* Called once per real opcode dispatch (never for a WAI continuation, which
 * never changes pc) with the (bank,pc) it dispatched from and the dots that
 * dispatch consumed. Attributes those dots to a poll group once a loop is
 * confirmed. */
static void poll_track(Snes *snes, uint32_t cur, int step) {
  int match_p = 0;
  for (int p = 1; p <= g_ring_len; p++) {
    if (g_pc_ring[p - 1] == cur) { match_p = p; break; }
  }
  for (int i = POLL_RING - 1; i > 0; i--) g_pc_ring[i] = g_pc_ring[i - 1];
  g_pc_ring[0] = cur;
  if (g_ring_len < POLL_RING) g_ring_len++;

  if (match_p > 0 && match_p == g_loop_period) {
    g_loop_hits++;
  } else if (match_p > 0) {
    g_loop_period = match_p; g_loop_hits = 1; g_loop_group = 0;
  } else {
    g_loop_period = 0; g_loop_hits = 0; g_loop_group = 0;
  }

  if (g_loop_period == 0 || g_loop_hits < POLL_CONFIRM_HITS) return;

  if (g_loop_group == 0) {
    uint32_t bank = cur >> 16;
    uint32_t minpc = 0xffff, maxpc = 0;
    bool same_bank = true;
    for (int i = 0; i < g_loop_period; i++) {
      uint32_t v = g_pc_ring[i];
      if ((v >> 16) != bank) { same_bank = false; break; }
      uint32_t pc = v & 0xffff;
      if (pc < minpc) minpc = pc;
      if (pc > maxpc) maxpc = pc;
    }
    if (!same_bank) { g_loop_group = 3; }
    else {
      uint32_t key = (bank << 16) | minpc;
      int slot = (int)(key % CLASSIFY_CACHE_SIZE);
      if (g_cls_used[slot] && g_cls_key[slot] == key) {
        g_loop_group = g_cls_group[slot];
      } else {
        int grp = classify_loop(snes, bank, (int)minpc, (int)maxpc);
        g_cls_used[slot] = true; g_cls_key[slot] = key; g_cls_group[slot] = (uint8_t)grp;
        g_loop_group = grp;
      }
    }
  }
  if (!g_wai_gate) return;
  switch (g_loop_group) {
    case 1: g_hv_dots += step; break;
    case 2: g_apu_dots += step; break;
    default: g_other_dots += step; break;
  }
}

/* ---- event loop, copied verbatim from snes_main.c (the parity oracle),
 * with per-quantum dead-wait sampling added to run_dots()/cpu_tick(). ------- */
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
    bool was_waiting = snes->cpu->waiting;
    uint32_t cur = ((uint32_t)snes->cpu->k << 16) | snes->cpu->pc;
    snes->cpuMemOps = 0;
    int cycles = cpu_runOpcode(snes->cpu);
    snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
    if (g_wai_gate) {
      g_total_ticks++; if (was_waiting) g_wai_ticks++;
      g_total_dots += 2; if (was_waiting) g_wai_dots += 2;
    }
    if (!was_waiting) poll_track(snes, cur, 2);
  } else if (g_wai_gate) {
    g_total_dots += 2;
  }
  snes->cpuCyclesLeft -= 2;
}
static void run_dots(Snes *snes, int dots) {
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma);
      snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2; dots -= 2;
      if (g_wai_gate) g_total_dots += 2;   /* DMA-busy dots are not dead-wait */
      continue;
    }
    bool started_dma = false;
    bool was_waiting = false;
    uint32_t cur = 0;
    if (snes->cpuCyclesLeft == 0) {
      apply_irq_match(snes);
      was_waiting = snes->cpu->waiting;    /* state entering this dispatch */
      cur = ((uint32_t)snes->cpu->k << 16) | snes->cpu->pc;
      snes->cpuMemOps = 0;
      int cycles = cpu_runOpcode(snes->cpu);
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
      if (g_wai_gate) { g_total_ticks++; if (was_waiting) g_wai_ticks++; }
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
    if (g_wai_gate) {
      g_total_dots += step;
      if (was_waiting) g_wai_dots += step;
    }
    if (!was_waiting && cur) poll_track(snes, cur, step);
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

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: snes_survey_wai <rom> [frames]\n"); return 2; }
  int frames = argc > 2 ? atoi(argv[2]) : 600;

  char pathcopy[1024];
  snprintf(pathcopy, sizeof(pathcopy), "%s", argv[1]);
  const char *base = basename(pathcopy);

  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("%s\tOPEN_FAIL\t-\t-\t-\t-\t-\t-\n", base); return 0; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0;
  fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n);
  if (fread(rom, 1, n, f) != (size_t)n) { fclose(f); printf("%s\tREAD_FAIL\t-\t-\t-\t-\t-\t-\n", base); return 0; }
  fclose(f);

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("%s\tLOAD_FAIL\t-\t-\t-\t-\t-\t-\n", base); return 0; }

  for (int i = 0; i < frames; i++) {
    g_wai_gate = (i >= FRAME_SKIP);
    snes->input1->currentState = 0;
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
    run_frame_events(snes);
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534) apu_cycle(snes->apu);
      snes->apu->dsp->sampleOffset = 0;   /* drain so the DSP keeps stepping */
    }
  }

  int lit = 0;
  for (int i = 0; i < 320 * 240; i++) if (g_fb[i]) lit++;

  /* All percentages share the same denominator (g_total_dots), so they are
   * apples-to-apples and (barring the loop-confirmation blind spot noted
   * above) should sum to well under 100%.
   *   WAI_DOT/WAI_TICK  -- dot-weighted / raw-dispatch-count % in WAI.
   *   HV_DOT            -- dot-weighted % spinning on $4210/11/12.
   *   APU_DOT           -- dot-weighted % spinning on $2140-43.
   *   OTHER_DOT         -- dot-weighted % in any other confirmed tight loop. */
  double pct_dot   = (g_total_dots  > 0) ? (100.0 * g_wai_dots   / g_total_dots) : -1.0;
  double pct_tick  = (g_total_ticks > 0) ? (100.0 * g_wai_ticks  / g_total_ticks) : -1.0;
  double pct_hv    = (g_total_dots  > 0) ? (100.0 * g_hv_dots    / g_total_dots) : -1.0;
  double pct_apu   = (g_total_dots  > 0) ? (100.0 * g_apu_dots   / g_total_dots) : -1.0;
  double pct_other = (g_total_dots  > 0) ? (100.0 * g_other_dots / g_total_dots) : -1.0;
  if (pct_dot < 0) {
    printf("%s\tOK\t%d\tWAI_DOT=NA\tWAI_TICK=NA\tHV_DOT=NA\tAPU_DOT=NA\tOTHER_DOT=NA\n", base, lit);
  } else {
    printf("%s\tOK\t%d\tWAI_DOT=%.1f\tWAI_TICK=%.1f\tHV_DOT=%.1f\tAPU_DOT=%.1f\tOTHER_DOT=%.1f\n",
           base, lit, pct_dot, pct_tick, pct_hv, pct_apu, pct_other);
  }
  return 0;
}
