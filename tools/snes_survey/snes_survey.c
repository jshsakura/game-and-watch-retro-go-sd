/* SNES sound-driver survey.
 *
 * Boots a ROM in the generic SNES core (external/sm/src/snes, device settings),
 * lets the game upload its sound driver into APU RAM, then scans that 64 KB ARAM
 * image for the VGMTrans-derived engine signatures in snes_driver_sigs.h and
 * reports which driver family(ies) it matched.
 *
 * The point: run this across a whole ROM library to learn how few driver
 * families cover most of it -- the "which drivers are worth HLE-ing" question
 * (see snes-core-feasibility). The frame loop is copied from snes_main.c (the
 * parity-checked event loop) because a driver is only in ARAM once the game has
 * actually run; we do not need the hash/PPM/WAV plumbing here.
 *
 * Output (one line per ROM, tab-separated, machine-readable):
 *   <rom-basename>\tOK\t<lit>\tfam:sig,fam:sig,...      -- matched
 *   <rom-basename>\tOK\t<lit>\t-                        -- booted, no driver matched
 *   <rom-basename>\tLOAD_FAIL\t-\t-                     -- mapper unsupported etc.
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
#include "snes_driver_sigs.h"

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

/* ---- event loop, copied verbatim from snes_main.c (the parity oracle) ------ */
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

/* ---- signature scan --------------------------------------------------------
 * VGMTrans mask: mask[i]=='x' -> ram[pos+i] must equal bytes[i]; else wildcard. */
static bool sig_matches_at(const uint8_t *ram, int pos, const DriverSig *s) {
  for (int i = 0; i < s->len; i++)
    if (s->mask[i] == 'x' && ram[pos + i] != s->bytes[i]) return false;
  return true;
}
static bool sig_found(const uint8_t *ram, const DriverSig *s) {
  int last = 0x10000 - s->len;
  for (int pos = 0; pos <= last; pos++)
    if (sig_matches_at(ram, pos, s)) return true;
  return false;
}

/* First ARAM offset where the signature named `name` matches, or -1. */
static int sig_pos_by_name(const uint8_t *ram, const char *name) {
  for (int i = 0; i < SNES_DRIVER_SIG_COUNT; i++) {
    const DriverSig *s = &SNES_DRIVER_SIGS[i];
    if (strcmp(s->name, name)) continue;
    int last = 0x10000 - s->len;
    for (int pos = 0; pos <= last; pos++)
      if (sig_matches_at(ram, pos, s)) return pos;
  }
  return -1;
}
static int rd8 (const uint8_t *r, int a) { return r[a & 0xffff]; }
static int rd16(const uint8_t *r, int a) { return r[a & 0xffff] | (r[(a + 1) & 0xffff] << 8); }

/* ---- N-SPC parameter recovery -------------------------------------------
 * Recover the three ARAM addresses Super Metroid's native player hardcodes --
 * song list (0x5820), instrument table (0x6C00), sample DIR -- from THIS game's
 * uploaded driver code, using VGMTrans's per-variant offset recipe
 * (NinSnesScanner.cpp). These three values are exactly what a generalized
 * spc_player needs to play an arbitrary N-SPC game. */
typedef struct { int songList, instrTab, dir, chOK; const char *variant; } NspcParams;

/* Walk song list -> song 0..N -> a section >= 0x100 -> 8 channel pattern
 * pointers, returning the best 0..8 count of plausible channel pointers. This
 * both validates a candidate song-list address and disambiguates which N-SPC
 * variant offset was right (the generic `std` pattern false-matches often, so we
 * score every candidate and keep the highest). */
static int nspc_score_songlist(const uint8_t *ram, int songList) {
  if (songList <= 0 || songList >= 0xfffe) return -1;
  int best = 0;
  for (int probe = 0; probe < 4; probe++) {
    int sec = rd16(ram, songList + probe * 2);
    if (sec < 0x100 || sec >= 0xfff0) continue;
    int t = sec, ok = 0;
    for (int ch = 0; ch < 8; ch++) {
      int cp = rd16(ram, t); t += 2;
      if (cp == 0 || (cp >= 0x100 && cp < 0xffff)) ok++;   /* 0 = silent channel, valid */
    }
    if (ok > best) best = ok;
  }
  return best;
}

static int nspc_extract(const uint8_t *ram, NspcParams *o) {
  o->songList = o->instrTab = o->dir = -1; o->chOK = -1; o->variant = "?";

  /* song list: readShort(ofsInitSectionPtr + variant offset). Try every variant
   * whose pattern is present and keep the one that dereferences best. */
  const struct { const char *name, *label; int off; } SL[] = {
    { "makeInitSectionPtrGD3Pattern","GD3", 8 }, { "makeInitSectionPtrYIPattern", "YI", 12 },
    { "makeInitSectionPtrSMWPattern","SMW", 3 }, { "makeInitSectionPtrPattern",   "std", 5 },
  };
  for (unsigned i = 0; i < sizeof(SL) / sizeof(SL[0]); i++) {
    int p = sig_pos_by_name(ram, SL[i].name);
    if (p < 0) continue;
    int cand = rd16(ram, p + SL[i].off);
    int score = nspc_score_songlist(ram, cand);
    if (score > o->chOK) { o->chOK = score; o->songList = cand; o->variant = SL[i].label; }
  }
  if (o->chOK < 0) return 0;
  if (o->chOK < 0) o->chOK = 0;

  /* instrument table: readByte(+lo) | readByte(+hi)<<8 */
  const struct { const char *name; int lo, hi; } IT[] = {
    { "ptnLoadInstrTableAddress", 7, 10 }, { "ptnLoadInstrTableAddressSMW", 3, 6 },
    { "ptnLoadInstrTableAddressCTOW", 7, 10 }, { "ptnLoadInstrTableAddressSOS", 1, 4 },
  };
  for (unsigned i = 0; i < sizeof(IT) / sizeof(IT[0]); i++) {
    int p = sig_pos_by_name(ram, IT[i].name);
    if (p >= 0) { o->instrTab = rd8(ram, p + IT[i].lo) | (rd8(ram, p + IT[i].hi) << 8); break; }
  }

  /* sample DIR (page-aligned; high byte only): readByte(+off) << 8 */
  const struct { const char *name; int off; } DIR[] = {
    { "ptnSetDIR", 4 }, { "ptnSetDIRYI", 1 }, { "ptnSetDIRSMW", 9 }, { "ptnSetDIRCTOW", 3 },
  };
  for (unsigned i = 0; i < sizeof(DIR) / sizeof(DIR[0]); i++) {
    int p = sig_pos_by_name(ram, DIR[i].name);
    if (p >= 0) { o->dir = rd8(ram, p + DIR[i].off) << 8; break; }
  }

  /* Validate the song list by dereferencing song 0 -> a section >= 0x100 ->
   * 8 per-channel pattern pointers. A recovered address that walks to plausible
   * channel pointers is strong evidence it is the real song table (and thus that
   * the native engine could be pointed at it). chOK is a soft 0..8 signal. */
  if (o->songList > 0 && o->songList < 0xfffe) {
    for (int probe = 0; probe < 4; probe++) {          /* skip leading control words */
      int sec = rd16(ram, o->songList + probe * 2);
      if (sec >= 0x100 && sec < 0xfff0) {
        int t = sec, ok = 0;
        for (int ch = 0; ch < 8; ch++) {
          int cp = rd16(ram, t); t += 2;
          if (cp == 0 || (cp >= 0x100 && cp < 0xffff)) ok++;   /* 0 = silent channel, valid */
        }
        if (ok > o->chOK) o->chOK = ok;
      }
    }
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: snes_survey <rom> [frames]\n"); return 2; }
  int frames = argc > 2 ? atoi(argv[2]) : 600;

  char pathcopy[1024];
  snprintf(pathcopy, sizeof(pathcopy), "%s", argv[1]);
  const char *base = basename(pathcopy);

  FILE *f = fopen(argv[1], "rb");
  if (!f) { printf("%s\tOPEN_FAIL\t-\t-\n", base); return 0; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  long hdr = (n % 1024 == 512) ? 512 : 0;
  fseek(f, hdr, SEEK_SET); n -= hdr;
  uint8_t *rom = malloc(n);
  if (fread(rom, 1, n, f) != (size_t)n) { fclose(f); printf("%s\tREAD_FAIL\t-\t-\n", base); return 0; }
  fclose(f);

  Snes *snes = snes_init(g_wram);
  g_the_snes = snes;
  if (!snes_loadRom(snes, rom, (int)n)) { printf("%s\tLOAD_FAIL\t-\t-\n", base); return 0; }

  for (int i = 0; i < frames; i++) {
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

  if (!snes->apu) { printf("%s\tNO_APU\t%d\t-\n", base, lit); return 0; }
  const uint8_t *ram = snes->apu->ram;
  { const char *dp = getenv("SNES_ARAMDUMP");
    if (dp) { FILE *df = fopen(dp, "wb"); if (df) { fwrite(ram, 1, 0x10000, df); fclose(df); } } }

  char matches[2048]; int mlen = 0; matches[0] = 0;
  for (int i = 0; i < SNES_DRIVER_SIG_COUNT; i++) {
    if (sig_found(ram, &SNES_DRIVER_SIGS[i])) {
      mlen += snprintf(matches + mlen, sizeof(matches) - mlen, "%s%s:%s",
                       mlen ? "," : "", SNES_DRIVER_SIGS[i].family, SNES_DRIVER_SIGS[i].name);
      if (mlen >= (int)sizeof(matches) - 64) break;
    }
  }
  /* For N-SPC hits, recover the per-ROM engine parameters (the SM-hardcoded
   * addresses generalized). 5th field: nspc:v=<variant>,song=,instr=,dir=,chOK=. */
  char params[128] = "-";
  if (mlen && strstr(matches, "Nin:")) {
    NspcParams np;
    if (nspc_extract(ram, &np)) {
      char sb[8], ib[8], db[8];
      if (np.songList >= 0) snprintf(sb, sizeof(sb), "%04x", np.songList & 0xffff); else strcpy(sb, "----");
      if (np.instrTab >= 0) snprintf(ib, sizeof(ib), "%04x", np.instrTab & 0xffff); else strcpy(ib, "----");
      if (np.dir      >= 0) snprintf(db, sizeof(db), "%04x", np.dir      & 0xffff); else strcpy(db, "----");
      snprintf(params, sizeof(params), "nspc:v=%s,song=%s,instr=%s,dir=%s,chOK=%d",
               np.variant, sb, ib, db, np.chOK);
    }
  }
  printf("%s\tOK\t%d\t%s\t%s\n", base, lit, mlen ? matches : "-", params);
  return 0;
}
