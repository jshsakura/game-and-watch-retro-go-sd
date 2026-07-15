/* sfc_recomp harness: tools/snes_harness/snes_main.c plus
 *   - a Start-tap input schedule so the game walks into gameplay
 *   - SNES_SITEDUMP/SNES_CARTDUMP (with -DSNES_PC_HISTOGRAM): write the executed
 *     opcode-site list and the live expanded cart image the translator folds
 *     constants from
 *   - RC_HYBRID: print the native/interpreter opcode split of the hybrid CPU
 * The run loop and hashes are IDENTICAL to the original so baseline-vs-hybrid
 * state hashes are comparable.
 *
 * Original header:
 * A general SNES emulator built from external/sm/src/snes — zero lines of Super
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
static uint64_t g_audiohash = 1469598103934665603ULL;
#ifdef SNES_PC_HISTOGRAM
uint32_t *g_pchist;   /* 16M entries, one per 24-bit opcode address (cpu.c bumps it) */
static void dump_pchist(void) {
  uint64_t total = 0;
  for (int i = 0; i < 0x1000000; i++) total += g_pchist[i];
  if (!total) { printf("[pchist] no samples\n"); return; }
  /* top-40 hottest opcode addresses */
  uint32_t top[40] = {0}; uint32_t topc[40] = {0};
  for (int i = 0; i < 0x1000000; i++) {
    uint32_t c = g_pchist[i];
    if (c <= topc[39]) continue;
    int j = 39; while (j > 0 && topc[j-1] < c) { topc[j]=topc[j-1]; top[j]=top[j-1]; j--; }
    topc[j] = c; top[j] = i;
  }
  printf("[pchist] total opcodes=%llu\n", (unsigned long long)total);
  /* per-bank rollup */
  uint64_t bank[256] = {0};
  for (int i = 0; i < 0x1000000; i++) bank[i >> 16] += g_pchist[i];
  printf("[pchist] hot banks (>=1%%):\n");
  for (int b = 0; b < 256; b++) if (bank[b] * 100 >= total)
    printf("   bank $%02x  %6.2f%%  (%llu)\n", b, 100.0*bank[b]/total, (unsigned long long)bank[b]);
  printf("[pchist] top-40 hot opcode addresses:\n");
  for (int k = 0; k < 40 && topc[k]; k++)
    printf("   $%02x:%04x  %6.3f%%  (%u)\n", top[k]>>16, top[k]&0xffff, 100.0*topc[k]/total, topc[k]);
}
#endif

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
/* Dump the 320x240 RGB565 framebuffer as a P6 PPM so a human can SEE that the
 * ROM actually renders — the state hash proves determinism, not that anything is
 * on screen. SNES_FRAMEDIR=<dir> SNES_FRAMEEVERY=<n> (default 60). */
static void dump_ppm(const uint16_t *fb, const char *dir, int idx) {
  char path[512];
  snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, idx);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n320 240\n255\n");
  for (int i = 0; i < 320 * 240; i++) {
    uint16_t px = fb[i];
    uint8_t r5 = (px >> 11) & 0x1f, g6 = (px >> 5) & 0x3f, b5 = px & 0x1f;
    uint8_t rgb[3] = { (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

/* Accumulate the mono 16 kHz stream and write a WAV so a human can JUDGE the
 * audio (SNES_WAV=<path>). The audio hash proves whether samples changed; only
 * ears say whether the change is acceptable. */
static int16_t g_wavbuf[16000 * 30];   /* up to 30 s */
static int g_wavlen = 0;
static void wav_append(const int16_t *s, int n) {
  for (int i = 0; i < n && g_wavlen < (int)(sizeof(g_wavbuf)/2); i++) g_wavbuf[g_wavlen++] = s[i];
}
static void wav_write(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  int rate = 16000, data = g_wavlen * 2;
  void *hdrw; (void)hdrw;
  #define W16(v) do { uint16_t x=(v); fwrite(&x,2,1,f); } while(0)
  #define W32(v) do { uint32_t x=(v); fwrite(&x,4,1,f); } while(0)
  fwrite("RIFF",1,4,f); W32(36 + data); fwrite("WAVE",1,4,f);
  fwrite("fmt ",1,4,f); W32(16); W16(1); W16(1); W32(rate); W32(rate*2); W16(2); W16(16);
  fwrite("data",1,4,f); W32(data); fwrite(g_wavbuf,1,data,f);
  fclose(f);
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
#ifdef SNES_PC_HISTOGRAM
  g_pchist = calloc(0x1000000, sizeof(uint32_t));
#endif

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < frames; i++) {
    /* Start-tap: walk title/menus into gameplay (same schedule as rig_snes.c's
     * RIG_INPUT_TAP). All builds of this harness use it, so hashes compare. */
    snes->input1->currentState = (i >= 40 && (i % 24) < 6) ? 0x0008 : 0;
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
      /* Fold the frame's audio into a running hash. The state hash (fb+wram+sram)
       * does NOT cover DSP output — a change that alters only the samples leaves it
       * identical. This makes an audio-only regression visible: a bit-identical DSP
       * optimization keeps BOTH hashes; a reduced-accuracy one moves audio only. */
      const uint8_t *ab = (const uint8_t *)g_audio;
      for (size_t q = 0; q < sizeof(g_audio); q++) { g_audiohash ^= ab[q]; g_audiohash *= 1099511628211ULL; }
      if (getenv("SNES_WAV")) wav_append(g_audio, 16000 / 60);
    }
    { const char *fd = getenv("SNES_FRAMEDIR");
      if (fd) { const char *ev = getenv("SNES_FRAMEEVERY");
        int every = ev ? atoi(ev) : 60;
        if (every > 0 && i % every == 0) dump_ppm(g_fb, fd, i); } }
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
  printf("%-10s %6.3f ms/frame  state=%016llx  audio=%016llx  lit=%d\n",
#ifdef SNES_DOT_LOOP
         "dot-loop",
#else
         "event",
#endif
         ms, (unsigned long long)hash_state(snes), (unsigned long long)g_audiohash, lit);
  { const char *wp = getenv("SNES_WAV"); if (wp) wav_write(wp); }
#ifdef SNES_PC_HISTOGRAM
  dump_pchist();
  /* Dump every executed opcode site (24-bit address) for the translator. */
  { const char *sp = getenv("SNES_SITEDUMP");
    if (sp && g_pchist) {
      FILE *sf = fopen(sp, "wb");
      if (sf) {
        uint32_t nsites = 0;
        for (uint32_t a = 0; a < (1u << 24); a++)
          if (g_pchist[a]) { fwrite(&a, 4, 1, sf); nsites++; }
        fclose(sf);
        fprintf(stderr, "[sitedump] %u sites -> %s\n", nsites, sp);
      }
    } }
  /* Dump the live expanded cart image + mapping params: the exact bytes and
   * formula snes_cpuRead's ROM fast path serves fetches from. */
  { const char *cp = getenv("SNES_CARTDUMP");
    if (cp) {
      FILE *cf = fopen(cp, "wb");
      if (cf) {
        fwrite(snes->cart->rom, 1, snes->cart->romSize, cf);
        fclose(cf);
        fprintf(stderr, "[cartdump] type=%d romSize=%u romMask=0x%08x -> %s\n",
                snes->cart->type, snes->cart->romSize, snes->cart->romMask, cp);
      }
    } }
#endif
#ifdef RC_HYBRID
  { extern uint64_t g_rc_native, g_rc_interp;
    double tot = (double)(g_rc_native + g_rc_interp);
    fprintf(stderr, "[rc] native=%llu interp=%llu coverage=%.4f%%\n",
            (unsigned long long)g_rc_native, (unsigned long long)g_rc_interp,
            tot > 0 ? 100.0 * g_rc_native / tot : 0.0); }
#endif
  return 0;
}
