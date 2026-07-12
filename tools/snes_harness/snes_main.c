/* A general SNES emulator built from external/sm/src/snes — zero lines of Super
 * Metroid — with the device's compile settings.
 *
 * Why this exists
 * ---------------
 * The SM port and this share a PPU, a DSP, a DMA unit and a timing core, byte for
 * byte. The only difference between them is that the port runs the game's 65816
 * code as native C and this interprets it. On hardware the port measures 56.2 fps;
 * this measures 4.6x heavier, which is ~12. That gap IS the interpreter, and it is
 * the whole argument for recompiling 65816 to C instead of interpreting it.
 *
 * What it is for
 * --------------
 *  1. A timing core to make fast (SNES_EVENT_LOOP, the default) without breaking it:
 *     the old per-dot walk is still here under SNES_DOT_LOOP, and the two must agree
 *     bit for bit — framebuffer, WRAM and SRAM are hashed and compared.
 *  2. A place to hang the recompiler's interpreter fallback off, later.
 *
 * Usage: snes_harness <rom> [frames]      prints a state hash and a frame time.
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

/* the firmware's allocators */
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *itc_malloc(size_t s) { return malloc(s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

/* The three hooks Super Metroid's reimplementation reaches into the emulator with.
 * A general core has no reimplementation, so they are inert — which is the point:
 * this build links no Super Metroid code at all. */
int  CpuOpcodeHook(uint32_t addr) { (void)addr; return 0; }
bool HookedFunctionRts(int level) { (void)level; return false; }
bool g_fail;
bool g_new_ppu = true;                 /* the fast line renderer, as on the device */

void Die(const char *s) { printf("Die: %s\n", s); exit(1); }
void Warning(const char *s) { (void)s; }

static Snes *g_the_snes;

/* Super Metroid routes APU port writes through its RTL so spc_player can hear them.
 * A general core has a real SPC700 sitting right there: catch it up and write to it.
 * (In a shipped core this belongs behind GNW_SNES_CORE inside snes_writeBBus; it
 * lives here so the core stays untouched while we measure.) */
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  /* The ports the SPC700 reads. NOT apu_cpuWrite() — that is the SPC's own bus
   * (0xf0-0xfc), and writing $2140 through it lands in APU RAM while inPorts stays
   * zero. The game then sits in the two-instruction loop at the top of every SNES
   * boot — STA $2140 / CMP $2140 / BNE — waiting for an echo that cannot come. */
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

static uint8_t  g_wram[0x20000];
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[16000 / 60];

static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---------------------------------------------------------------- dot loop ---
 * The reference. Two dots per call, 178,684 calls a frame, and all but ~800 of
 * them do nothing but increment a counter. Kept as the oracle the event loop is
 * checked against — never delete it. */
static void run_frame_dots(Snes *snes) {
  while (snes->inVblank || !snes->inVblank) {
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);

    if (!dma_cycle(snes->dma)) {
      if (snes->cpuCyclesLeft == 0) {
        snes->cpuMemOps = 0;
        int cycles = cpu_runOpcode(snes->cpu);
        /* every memory op already charged 8 in snes_cpuRead/Write; the rest cost 6 */
        snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      }
      snes->cpuCyclesLeft -= 2;
    }

    if (snes->hPos == 0 && snes->vPos == 0) break;   /* frame wrapped */
  }
  snes_catchupApu(snes);
}

/* ------------------------------------------------------------- event loop ---
 * Same machine, without the dead steps.
 *
 * Between the three dot positions that do anything (0, 512, 1024) nothing observes
 * hPos: cpu_runOpcode() is atomic, so the CPU sees one hPos for the whole
 * instruction and then idles for cpuCyclesLeft dots. Those idle dots can be
 * consumed in one subtraction instead of one loop iteration each — and after an
 * opcode cpuCyclesLeft is typically 12-48, so that is 6-24 iterations collapsed
 * into one. The events fire at exactly the same hPos, the opcodes start at exactly
 * the same hPos, and the state hash comes out identical.
 *
 * An armed H-timer can fire on any dot, so when one is armed we stop bulking and
 * step it out. Same rule the SM port uses. */
static int dots_to_next_event(Snes *snes) {
  int h = snes->hPos;

  /* We may already be standing on one — the line wraps to hPos 0, which is the
   * biggest event dot there is. Return 0 and let the caller handle it. */
  if (h == 0 || h == 512 || h == 1024) return 0;
  if (snes->hIrqEnabled && h == snes->hTimer * 4) return 0;

  /* 1362, not 1364: the line wraps inside snes_handle_pos_stuff() when it is
   * called at 1362 and steps hPos to 1364. Land on 1364 ourselves and nothing
   * wraps it — hPos runs away and vPos never advances again. */
  int next = 1362;                              /* last dot of the line */
  if (h < 512)       next = 512;
  else if (h < 1024) next = 1024;

  if (snes->hIrqEnabled) {
    int t = snes->hTimer * 4;
    if (t > h && t < next) next = t;
  }
  return next - h;
}

/* The dot loop re-asserts the timer IRQ on every single dot the condition holds —
 * a V-timer with no H-timer matches all 682 dots of its line. That is not just
 * noise: the game acknowledges by reading $4211, which clears irqWanted, and the
 * very next dot sets it again. Only cpu_runOpcode() ever reads the flag, and only
 * at the start of an instruction, so asserting it once before each opcode is the
 * same machine — and costs one check per opcode instead of one per dot. */
static void apply_irq_match(Snes *snes) {
  if (!(snes->hIrqEnabled || snes->vIrqEnabled)) return;
  if (snes->vIrqEnabled && snes->vPos != snes->vTimer) return;
  if (snes->hIrqEnabled && snes->hPos != snes->hTimer * 4) return;
  snes->inIrq = true;
  snes->cpu->irqWanted = true;
}

/* Exactly what the dot loop does to the CPU in one 2-dot tick, minus the hPos
 * bookkeeping (the caller owns that). */
static void cpu_tick(Snes *snes) {
  if (dma_cycle(snes->dma))
    return;
  if (snes->cpuCyclesLeft == 0) {
    snes->cpuMemOps = 0;
    int cycles = cpu_runOpcode(snes->cpu);
    snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
  }
  snes->cpuCyclesLeft -= 2;   /* uint8_t: the reference wraps here too, so we must */
}

/* Advance `dots` dots with no event in them. */
static void run_dots(Snes *snes, int dots) {
  while (dots > 0) {
    if (snes->dma->dmaBusy || snes->dma->hdmaTimer > 0) {
      dma_cycle(snes->dma);
      snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      snes->hPos += 2;
      dots -= 2;
      continue;
    }

    bool started_dma = false;
    if (snes->cpuCyclesLeft == 0) {
      apply_irq_match(snes);
      snes->cpuMemOps = 0;
      int cycles = cpu_runOpcode(snes->cpu);
      snes->cpuCyclesLeft += (cycles - snes->cpuMemOps) * 6;
      /* An opcode can start a DMA (a write to $420B/$420C). From the next dot on,
       * the DMA unit owns the bus and the CPU's countdown stops — so there is
       * nothing to bulk. Take one tick and come back through the DMA branch. */
      started_dma = snes->dma->dmaBusy || snes->dma->hdmaTimer > 0;
    }

    /* The opcode has run; what is left is idle dots. Charge them all at once —
     * this is the entire trick. Only bulk when there are at least 2 to bulk:
     * cpuCyclesLeft is a uint8_t and the reference underflows it when an opcode
     * charges nothing, so that case has to be stepped, wrap and all. */
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
    snes->hPos += step;
    dots -= step;
  }
}

static void run_frame_events(Snes *snes) {
  for (;;) {
    /* the event dot itself: same order as the dot loop — events, then the CPU */
    snes->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(snes);       /* fires 0/512/1024, advances hPos by 2, wraps */
    cpu_tick(snes);
    if (snes->hPos == 0 && snes->vPos == 0) break;   /* frame wrapped */

    /* then straight to the next dot that does anything */
    run_dots(snes, dots_to_next_event(snes));
  }
  snes_catchupApu(snes);
}

/* ------------------------------------------------------------------- main --- */
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
  if (argc < 2) { printf("usage: snes_harness <rom> [frames]\n"); return 1; }
  int frames = argc > 2 ? atoi(argv[2]) : 900;

  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("no rom: %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0;      /* copier header */
  fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n);
  if (fread(rom, 1, n, f) != (size_t)n) return 1;
  fclose(f);

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("unsupported ROM\n"); return 1; }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < frames; i++) {
    snes->input1->currentState = 0;
    PpuBeginDrawing(snes->ppu, (uint8_t *)(g_fb + 32), 320 * 2, 0);
#ifdef SNES_DOT_LOOP
    run_frame_dots(snes);
#else
    run_frame_events(snes);
#endif
    if (snes->apu) {
      while (snes->apu->dsp->sampleOffset < 534)
        apu_cycle(snes->apu);
      dsp_getSamples(snes->apu->dsp, g_audio, 16000 / 60, 1);
    }
    if (getenv("SNES_DBG") && i % 120 == 0) {
      int wram_nz = 0;
      for (int q = 0; q < 0x20000; q++) if (g_wram[q]) wram_nz++;
      fprintf(stderr, "f%4d pc=%02x:%04x nmiEn=%d fblank=%d wramNZ=%d | apu: pc=%04x out=%02x %02x %02x %02x in=%02x %02x %02x %02x\n",
              i, snes->cpu->k, snes->cpu->pc, snes->nmiEnabled, snes->ppu->forcedBlank, wram_nz,
              snes->apu ? snes->apu->spc->pc : 0,
              snes->apu ? snes->apu->outPorts[0] : 0, snes->apu ? snes->apu->outPorts[1] : 0,
              snes->apu ? snes->apu->outPorts[2] : 0, snes->apu ? snes->apu->outPorts[3] : 0,
              snes->apu ? snes->apu->inPorts[0] : 0, snes->apu ? snes->apu->inPorts[1] : 0,
              snes->apu ? snes->apu->inPorts[2] : 0, snes->apu ? snes->apu->inPorts[3] : 0);
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);

  double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames;
  int lit = 0;
  for (int i = 0; i < 320 * 240; i++) if (g_fb[i]) lit++;
  printf("%-10s %6.3f ms/frame  state=%016llx  lit=%d\n",
#ifdef SNES_DOT_LOOP
         "dot-loop",
#else
         "event",
#endif
         ms, (unsigned long long)hash_state(snes), lit);
  return 0;
}
