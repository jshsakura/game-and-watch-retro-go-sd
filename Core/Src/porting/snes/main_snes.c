/* Generic SNES core (LakeSnes interpreter, external/sm/src/snes) as a launcher
 * system — EXPERIMENTAL baseline.
 *
 * Milestone 1: interpreter + the PPU line-renderer optimizations, nothing else.
 * The measured M7-rig figure for this exact configuration is 49.2 fps (Zelda,
 * gameplay); the first real-hardware number from this build calibrates the
 * rig's insn-count against device cycles for every further lever (spin-skip,
 * translator/XIP, audio HLE), which land separately.
 *
 * The frame loop below (dots_to_next_event / run_dots / run_frame_events) is
 * the parity-proven event loop from tools/snes_harness/snes_main.c — it and
 * the per-dot reference walk produce bit-identical state hashes there. Do not
 * "improve" it here without re-running that oracle.
 *
 * LoROM/HiROM only. Enhancement-chip carts (SA-1, SuperFX, DSP-1, ...) are
 * rejected at load with a message instead of a mid-game Hardfault.
 */
#include <odroid_system.h>

#include <assert.h>
#include <string.h>
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "rom_manager.h"
#include "common.h"
#include "gw_malloc.h"
#include "rg_storage.h"
#include "odroid_overlay.h"
#include "appid.h"
#include "rg_i18n.h"
#include "gw_flash_alloc.h"

#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/input.h"
#include "snes/saveload.h"
#include "snes/spin_skip.h"
#include "snes/rc_dispatch.h"
#include "crc32.h"

bool snes_loadRom(Snes *snes, const uint8_t *data, int length);   /* snes_other.c */

#define SNES_FPS            60
#define SNES_WIDTH          256
#define SNES_HEIGHT         224
#define SNES_AUDIO_RATE     16000
#define SNES_AUDIO_SAMPLES  (SNES_AUDIO_RATE / SNES_FPS)   /* 266/frame */

/* Savestate stamp: a raw struct dump must refuse files this build didn't
 * write (project rule — a stale state "loads" and restores nonsense). */
#define SNES_STATE_MAGIC    0x31534E53u   /* "SNS1" */
#define SNES_STATE_VERSION  2   /* v2: + controller shift registers (see below) */

/* ---- hooks the snes lib links against ------------------------------------
 * These are the Super Metroid RTL hooks; a generic core has no reimplementation
 * so they are inert (same shims as the host harness / M7 rig). They live in
 * this object and get namespaced together with the rest of the overlay by
 * snes_redefines, so they cannot alias the SM port's real ones. */
int  CpuOpcodeHook(uint32_t addr) { (void)addr; return 0; }
bool HookedFunctionRts(int level) { (void)level; return false; }
bool g_fail;
bool g_new_ppu = true;                 /* the fast line renderer, as measured */

static Snes *g_the_snes;

/* The lib routes $2140-43 writes through this (snes.c). Catch the APU up and
 * write the CPU-visible mailbox — NOT apu_cpuWrite(), which is the SPC's own
 * bus and would leave inPorts stale (boot then spins on the port echo). */
void RtlApuWrite(uint32_t adr, uint8_t val) {
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
}

void Die(const char *s) {
  printf("SNES Die: %s\n", s);
  assert(!"snes core died");
}
void Warning(const char *s) { (void)s; }

extern bool g_ppu_skip_render;   /* ppu.c: skip compositing on dropped frames */

/* ---- state ---------------------------------------------------------------- */
static Snes *snes;
/* 128 KB WRAM lives in the overlay BSS (like the SM port's g_ram): the AHB
 * pool is only 120 KB total and the Apu (~66 KB) already comes from it. */
static uint8_t snes_wram[0x20000];
static int16_t audio_buf[SNES_AUDIO_SAMPLES];  /* mono frame mix from the DSP  */

static const double apuCyclesPerMaster = (32040 * 32) / (1364 * 262 * 60.0);

/* ---- event loop (verbatim from tools/snes_harness/snes_main.c) ------------ */
static int dots_to_next_event(Snes *s) {
  int h = s->hPos;
  if (h == 0 || h == 512 || h == 1024) return 0;
  if (s->hIrqEnabled && h == s->hTimer * 4) return 0;
  int next = 1362;
  if (h < 512)       next = 512;
  else if (h < 1024) next = 1024;
  if (s->hIrqEnabled) {
    int t = s->hTimer * 4;
    if (t > h && t < next) next = t;
  }
  return next - h;
}

static void apply_irq_match(Snes *s) {
  if (!(s->hIrqEnabled || s->vIrqEnabled)) return;
  if (s->vIrqEnabled && s->vPos != s->vTimer) return;
  if (s->hIrqEnabled && s->hPos != s->hTimer * 4) return;
  s->inIrq = true;
  s->cpu->irqWanted = true;
}

/* One real interpreter call, with the spin learner watching (harness-identical:
 * tools/snes_spin compiles the same spin_skip.c and gates skip-off vs skip-on to
 * bit-identical state+audio hashes). */
static int run_one_opcode(Snes *s) {
  Cpu *cpu = s->cpu;
  uint32_t pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
  int disp = (cpu->nmiWanted || (cpu->irqWanted && !cpu->i) || cpu->waiting) && !cpu->stopped;
  s->cpuMemOps = 0;
  int cycles = cpu_runOpcode(cpu);
  s->cpuCyclesLeft += (cycles - s->cpuMemOps) * 6;
  g_spin.ops_real++;
  spin_note(cpu, pc24, (uint8_t)s->cpuCyclesLeft, disp);
  return cycles;
}

static void cpu_tick(Snes *s) {
  if (dma_cycle(s->dma)) return;
  if (s->cpuCyclesLeft == 0) run_one_opcode(s);
  s->cpuCyclesLeft -= 2;
}

static void run_dots(Snes *s, int dots) {
  Cpu *cpu = s->cpu;
  while (dots > 0) {
    if (s->dma->dmaBusy || s->dma->hdmaTimer > 0) {
      dma_cycle(s->dma);
      s->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      s->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (s->cpuCyclesLeft == 0) {
      /* Replay branch: a learned pure wait-loop iteration is a no-op — charge
       * the recorded cycle pattern and park the pc where the real call would
       * have, WITHOUT the interpreter. Falls through to the shared bulk-consume
       * so hPos steps and the apuCatchupCycles FMA sequence stay bit-identical. */
      if (g_spin.on &&
          !cpu->nmiWanted && !cpu->irqWanted && !cpu->waiting && !cpu->stopped &&
          !s->hIrqEnabled &&
          !(s->vIrqEnabled && s->vPos == s->vTimer) &&
          (((uint32_t)cpu->k << 16) | cpu->pc) == g_spin.pc[g_spin.idx]) {
        s->cpuCyclesLeft += g_spin.charge[g_spin.idx];
        g_spin.idx = (g_spin.idx + 1) % g_spin.len;
        cpu->k  = (uint8_t)(g_spin.pc[g_spin.idx] >> 16);
        cpu->pc = (uint16_t)g_spin.pc[g_spin.idx];
        g_spin.ops_virtual++;
      } else {
        apply_irq_match(s);
        run_one_opcode(s);
        started_dma = s->dma->dmaBusy || s->dma->hdmaTimer > 0;
      }
    }
    int step;
    if (s->cpuCyclesLeft >= 2 && !started_dma) {
      step = s->cpuCyclesLeft;
      if (step > dots) step = dots;
      step &= ~1;
      s->cpuCyclesLeft -= (uint8_t)step;
    } else {
      step = 2;
      s->cpuCyclesLeft -= 2;
    }
    s->apuCatchupCycles += apuCyclesPerMaster * step;
    s->hPos += step; dots -= step;
  }
}

static void run_frame_events(Snes *s) {
  for (;;) {
    s->apuCatchupCycles += apuCyclesPerMaster * 2.0;
    snes_handle_pos_stuff(s);
    cpu_tick(s);
    if (s->hPos == 0 && s->vPos == 0) break;
    run_dots(s, dots_to_next_event(s));
  }
  snes_catchupApu(s);
  spin_frame_tick();   /* auto-gate: park the learner on non-spinning carts */
}

/* ---- input ----------------------------------------------------------------
 * LakeSnes input1->currentState bit layout (auto-joypad order):
 * 0=B 1=Y 2=Select 3=Start 4=Up 5=Down 6=Left 7=Right 8=A 9=X 10=L 11=R.
 * Two face buttons on the unit: A→A, B→B; GAME→X, TIME→Y so ALttP's map/item
 * screens stay reachable; START/SELECT as themselves (Zelda-edition buttons). */
static uint16_t read_snes_pad(odroid_gamepad_state_t *joy) {
  uint16_t s = 0;
  if (joy->values[ODROID_INPUT_B])      s |= 1u << 0;
  if (joy->values[ODROID_INPUT_Y])      s |= 1u << 1;   /* TIME  = SNES Y      */
  if (joy->values[ODROID_INPUT_SELECT]) s |= 1u << 2;
  if (joy->values[ODROID_INPUT_START])  s |= 1u << 3;
  if (joy->values[ODROID_INPUT_UP])     s |= 1u << 4;
  if (joy->values[ODROID_INPUT_DOWN])   s |= 1u << 5;
  if (joy->values[ODROID_INPUT_LEFT])   s |= 1u << 6;
  if (joy->values[ODROID_INPUT_RIGHT])  s |= 1u << 7;
  if (joy->values[ODROID_INPUT_A])      s |= 1u << 8;
  if (joy->values[ODROID_INPUT_X])      s |= 1u << 9;   /* GAME  = SNES X      */
  return s;
}

/* ---- video ----------------------------------------------------------------
 * The line renderer writes RGB565. 256 px wide into the 320x240 panel with a
 * 32 px left margin and 8 px top margin — vertically centred (matches the SM
 * homebrew placement in main_sm.c). SNES renders up to 224 visible lines; the
 * row guard clips any overscan that would run past the framebuffer. */
/* Video. The PPU renders one line at a time into snes_line (renderPitch 0 → every
 * line lands in the same scratch) and hands it to g_ppu_line_cb — the contract the
 * recent PPU refactor (681371b) introduced and the SM port already uses. We place
 * each line into a PRIVATE persistent framebuffer (snes_frame), then copy the whole
 * frame to the LCD's active buffer at present time. That is the robust pattern GBA
 * uses: the LCD is double-buffered, and painting a full frame into whichever buffer
 * is active each present keeps BOTH buffers complete — rendering per-line straight
 * into the active buffer left the OTHER buffer stale (black on the device, strobing
 * after an overlay toggle). 256 px wide, 32 px left margin, 8 px top margin; the
 * private buffer's borders are cleared once and stay black. */
#define SNES_TOP_MARGIN  ((GW_LCD_HEIGHT - SNES_HEIGHT) / 2)   /* (240-224)/2 = 8 */
#define SNES_LEFT_MARGIN ((GW_LCD_WIDTH - SNES_WIDTH) / 2)     /* (320-256)/2 = 32 */
static uint16_t snes_line[256];
static uint16_t snes_frame[GW_LCD_WIDTH * GW_LCD_HEIGHT];

static void snes_blit_line(unsigned y, const uint16_t *line) {
  if (y < 1) return;   /* y is 1-based */
  unsigned row = (y - 1) + SNES_TOP_MARGIN;
  if (row >= GW_LCD_HEIGHT) return;   /* clip overscan past the panel */
  memcpy(snes_frame + row * GW_LCD_WIDTH + SNES_LEFT_MARGIN, line, sizeof(snes_line));
}

static void render_frame_into_active_buffer(void) {
  g_ppu_line_cb = &snes_blit_line;
  PpuBeginDrawing(snes->ppu, (uint8_t *)snes_line, 0, 0);  /* pitch 0: every line here */
}

static void present_frame(void) {
  memcpy(lcd_get_active_buffer(), snes_frame, sizeof(snes_frame));
}

/* Present the last rendered frame and draw the in-game overlay on top. Used both
 * as the normal per-frame present and as the overlay's repaint callback, so the
 * pause menu keeps the game behind it instead of a stale/black background. */
static void blit(void) {
  present_frame();
  common_ingame_overlay();
}

/* ---- audio ----------------------------------------------------------------
 * Top the DSP up to one frame of samples (534 stereo pairs internally) and
 * downmix to 16 kHz mono, exactly like the harness/rig. */
static void snes_pcm_submit(void) {
  if (snes->apu) {
    while (snes->apu->dsp->sampleOffset < 534)
      apu_cycle(snes->apu);
    dsp_getSamples(snes->apu->dsp, audio_buf, SNES_AUDIO_SAMPLES, 1);
  } else {
    memset(audio_buf, 0, sizeof(audio_buf));
  }

  int16_t *dst = audio_get_active_buffer();
  uint16_t dst_len = audio_get_buffer_length();
  if (common_emu_sound_loop_is_muted())
    return;
  int32_t factor = common_emu_sound_get_volume();
  uint16_t n = dst_len < SNES_AUDIO_SAMPLES ? dst_len : SNES_AUDIO_SAMPLES;
  for (uint16_t i = 0; i < n; i++)
    dst[i] = (int16_t)(((int32_t)audio_buf[i] * factor) >> 8);
  for (uint16_t i = n; i < dst_len; i++)
    dst[i] = 0;
}

/* ---- savestate -------------------------------------------------------------
 * snes_saveload() streams every subsystem (cpu/apu+dsp/dma/ppu/cart-sram/wram)
 * through one SaveLoadFunc; we stream it straight to the file behind a stamp.
 * ppu_saveload rebuilds its derived caches (palette, sprite-line cache) on
 * load, so no manual invalidation is needed here. */
static FILE *state_file;
static uint32_t state_bytes;

static void state_write(void *ctx, void *data, size_t size) {
  (void)ctx;
  wdog_refresh();
  if (state_file)
    fwrite(data, 1, size, state_file);
  state_bytes += size;
}

static void state_read(void *ctx, void *data, size_t size) {
  (void)ctx;
  wdog_refresh();
  size_t got = state_file ? fread(data, 1, size, state_file) : 0;
  if (got < size)
    memset((uint8_t *)data + got, 0, size - got);
  state_bytes += size;
}

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t length;    /* payload bytes after this header */
} snes_state_header_t;

/* The lib chain (snes_saveload) covers cpu/apu+dsp/dma/ppu/cart/wram but NOT
 * the controller shift registers (Input.latchLine/latchedState) — SM never
 * reads a port serially so the lib never needed them. Real games do: DKC
 * manual-reads $4016 and a cold resume handed it a zeroed shift register
 * where the live machine returns 1s after the auto-joy shift-out (found by
 * the two-process cold-resume proof, tools/snes_save_test). Serialize them
 * here, after the lib stream, without touching the lib. */
static void state_io_input(SaveLoadFunc *func) {
  Input *pads[2] = { snes->input1, snes->input2 };
  for (int i = 0; i < 2; i++) {
    func(NULL, &pads[i]->latchLine, sizeof(pads[i]->latchLine));
    func(NULL, &pads[i]->latchedState, sizeof(pads[i]->latchedState));
  }
}

static void state_stream(SaveLoadFunc *func) {
  snes_saveload(snes, func, NULL);
  state_io_input(func);
}

static bool snes_SaveState(const char *pathName) {
  /* Pass 1: count. Pass 2: write behind an accurate header. */
  state_file = NULL; state_bytes = 0;
  state_stream(&state_write);
  uint32_t payload = state_bytes;

  FILE *f = fopen(pathName, "wb");
  if (!f) return false;
  snes_state_header_t h = { SNES_STATE_MAGIC, SNES_STATE_VERSION, payload };
  if (fwrite(&h, 1, sizeof(h), f) != sizeof(h)) { fclose(f); return false; }
  state_file = f; state_bytes = 0;
  state_stream(&state_write);
  fclose(f);
  state_file = NULL;
  return state_bytes == payload;
}

static bool snes_LoadState(const char *pathName) {
  FILE *f = fopen(pathName, "rb");
  if (!f) return false;
  snes_state_header_t h;
  if (fread(&h, 1, sizeof(h), f) != sizeof(h) ||
      h.magic != SNES_STATE_MAGIC || h.version != SNES_STATE_VERSION) {
    /* Not ours / other build: refuse rather than restore nonsense. */
    fclose(f);
    return false;
  }
  /* Refuse BEFORE touching the machine, not after: (a) the payload must be
   * exactly the size this build streams (a lying length would otherwise be
   * caught only after the machine is clobbered), (b) the file must actually
   * contain it (state_read zero-fills past EOF, so a truncated file would
   * "load" a half-zeroed machine and report success — proven by the refusal
   * test before this check existed). */
  /* Dry run with the WRITE counter (file==NULL: counts, reads nothing into
   * the machine — state_read would zero-fill the live machine here). */
  state_file = NULL; state_bytes = 0;
  state_stream(&state_write);          /* what this build expects, in bytes */
  uint32_t expected = state_bytes;
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  if (h.length != expected ||
      fsize != (long)(sizeof(h) + expected)) {
    fclose(f);
    return false;
  }
  fseek(f, sizeof(h), SEEK_SET);
  state_file = f; state_bytes = 0;
  state_stream(&state_read);
  fclose(f);
  state_file = NULL;
  lcd_clear_active_buffer();
  /* A load replaces the whole machine: a learned spin pattern (and its purity
   * sequence history) now describes a machine that no longer exists. Relearn. */
  spin_reset();
  return state_bytes == h.length;
}

static void *snes_Screenshot(void) {
  lcd_wait_for_vblank();
  return lcd_get_active_buffer();
}

/* ---- ROM loading -----------------------------------------------------------
 * The cart stays memory-mapped in external flash (flash-cache machinery); the
 * core only reads it. Copier headers (512 bytes) are skipped in place. */
static const uint8_t *snes_rom;
static uint32_t snes_rom_len;

/* $ffd6 (ROM type): 0=ROM 1=ROM+RAM 2=ROM+RAM+battery; 3+ = coprocessor
 * (DSP-x/SA-1/SuperFX/...) which this core cannot run yet. Find the header the
 * same way the loader scores it: the offset whose checksum ^ complement is
 * 0xFFFF wins; if neither validates, let snes_loadRom decide. */
static bool cart_needs_coprocessor(const uint8_t *rom, uint32_t len) {
  static const uint32_t offs[2] = { 0x7fb0, 0xffb0 };   /* LoROM, HiROM */
  for (int i = 0; i < 2; i++) {
    if (offs[i] + 0x30 > len) continue;
    const uint8_t *h = rom + offs[i];
    uint16_t cks  = h[0x2e] | (h[0x2f] << 8);
    uint16_t icks = h[0x2c] | (h[0x2d] << 8);
    if ((cks ^ icks) == 0xffff)
      return h[0x26] >= 0x03;    /* $ffd6 = header+0x26 */
  }
  return false;
}

/* ---- rc SMW native optimization ---------------------------------------------
 * Per-ROM static recompilation: SMW's 270 hottest 65816 sites translated to C.
 * The overlay's cpu_runOpcode fast path (g_rc_active) dispatches to sites
 * instead of interpreting. Rig-measured: -42.3% insn/frame on SMW.
 *
 * ITCM-resident: the rc hot subset (270 sites, ~12 KB) is linked at ITCM VMA
 * and copied there by run_internal_emu BEFORE app_main runs. Zero wait-state
 * execution — no QSPI cache, no sentinel patching, no XIP thrash.
 * rc takes priority over spin-skip (rc replaces the interpreter entirely). */
#define RC_SMW_MAGIC     0x4D534352u   /* 'RCSM' little-endian — blob header */

/* Diag: code-region hash computed at activation time, reported in profile2 so
 * a mismatch (user's dump has different code at translated PCs) is visible
 * without a debugger. */
static uint32_t s_rc_diag_crc = 0;   /* computed code hash (name kept for diag fmt) */
static uint32_t s_rc_diag_exp = 0;   /* expected code hash from blob header */

/* Blob header layout (matches rc_smw_sites.c's rc_smw_header). Pointer fields
 * are linked at ITCM VMA directly — no sentinel patching needed. */
typedef struct {
  uint32_t magic;
  uint32_t nsites;
  uint32_t code_hash;   /* FNV-1a of consumed bytes (opcode+operands) at all site PCs */
  const uint32_t *addrs;   /* ITCM VMA of rc_addrs[] */
  const void **fns;        /* ITCM VMA of rc_fns[] */
  const uint8_t *lens;     /* ITCM VMA of rc_site_lens[] */
} rc_smw_header_t;

/* The header symbol — defined in rc_smw_sites.c, linked into .itcm_rc_hot
 * (ITCM VMA). Available as a direct extern because run_internal_emu copies the
 * ITC blob to ITCM before app_main runs. */
extern const rc_smw_header_t rc_smw_header;

/* SNES-overlay-resident hash storage (defined in rc_smw_sites.c, linked into
 * .overlay_snes_bss). Passed to rc_dispatch_init() so it never touches the
 * 81 KB DTCM heap. */
extern rc_entry_t rc_hash_storage[];
extern uint32_t rc_bank_off[];
extern uint32_t rc_bank_mask[];

/* SMW title hash (FNV-1a of 21-byte internal title at LoROM 0x7FC0).
 * Same value the spin-skip whitelist uses — quick reject for non-SMW ROMs. */
#define RCSMW_TITLE_HASH  0xFB0BD0ECu

static bool rc_smw_activate(const uint8_t *rom, uint32_t len) {
#if RCSMW
  if (len == 0 || rom == NULL) return false;

  /* Quick reject: SMW internal title hash (21 bytes at LoROM 0x7FC0).
   * Same FNV-1a the spin-skip whitelist uses — cheap reject for non-SMW ROMs. */
  if (len < 0x7FD5) return false;
  uint32_t th = 0x811C9DC5u;
  for (int i = 0; i < 21; i++) {
    th ^= rom[0x7FC0 + i];
    th *= 0x01000193u;
  }
  if (th != RCSMW_TITLE_HASH) {
    printf("rc_smw: not SMW (title hash %08lX != %08lX)\n",
           (unsigned long)th, (unsigned long)RCSMW_TITLE_HASH);
    return false;
  }

  /* The header is a direct symbol at ITCM VMA — code already copied by
   * run_internal_emu. No fopen/QSPI cache/sentinel patching. */
  const rc_smw_header_t *hdr = &rc_smw_header;
  if (hdr->magic != RC_SMW_MAGIC || hdr->nsites == 0) {
    printf("rc_smw: bad header (magic=0x%08lX nsites=%lu)\n",
           (unsigned long)hdr->magic, (unsigned long)hdr->nsites);
    return false;
  }

  /* Code-region hash gate: FNV-1a of consumed bytes (opcode + operands) at
   * every translated site PC. "The bytes are identity" — same principle as
   * GBA M4A HLE. Accepts any dump/patch/hack whose CODE at the translated PCs
   * is byte-identical to the reference dump; rejects any code change.
   * Text/graphics hacks (different data, same code) pass correctly. */
  uint32_t rom_mask = len - 1;
  uint32_t ch = 0x811C9DC5u;
  for (uint32_t i = 0; i < hdr->nsites; i++) {
    uint32_t a = hdr->addrs[i];
    uint8_t bank = a >> 16;
    uint16_t off = a & 0xFFFF;
    uint32_t idx = ((uint32_t)(bank & 0x7F) << 15) | (off & 0x7FFF);
    int nbytes = 1 + hdr->lens[i];   /* opcode + operand bytes */
    for (int j = 0; j < nbytes; j++) {
      ch ^= rom[(idx + j) & rom_mask];
      ch *= 0x01000193u;
    }
  }
  s_rc_diag_crc = ch;
  s_rc_diag_exp = hdr->code_hash;
  if (ch != hdr->code_hash) {
    printf("rc_smw: code hash mismatch (got %08lX want %08lX) — not activating\n",
           (unsigned long)ch, (unsigned long)hdr->code_hash);
    return false;
  }

  /* Build the dispatch table. rc_fns[] pointers are at ITCM VMA (linked
   * directly, no patching). The hash storage lives in SNES overlay BSS
   * (rc_smw_sites.c) — NOT the DTCM heap. After this call, g_rc_active is
   * true and cpu_runOpcode uses the rc fast path. */
  rc_dispatch_init(rc_hash_storage, rc_bank_off, rc_bank_mask,
                   hdr->addrs, hdr->nsites, (void (**)(Cpu *))hdr->fns);
  printf("rc_smw: activated — %lu sites, ITCM header at %p\n",
         (unsigned long)hdr->nsites, hdr);
  return true;
#else
  (void)rom; (void)len;
  return false;
#endif
}

void app_main_snes(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
  odroid_gamepad_state_t joystick;
  odroid_dialog_choice_t options[] = {
      ODROID_DIALOG_CHOICE_LAST
  };

  if (start_paused) {
    common_emu_state.pause_after_frames = 2;
    odroid_audio_mute(true);
  } else {
    common_emu_state.pause_after_frames = 0;
  }
  common_emu_state.frame_time_10us = (uint16_t)(100000 / SNES_FPS + 0.5f);
  lcd_set_refresh_rate(SNES_FPS);

  /* Interpreter-baseline is CPU-bound (rig: ~6.9M insn/frame vs 4.7M budget
   * at 280 MHz) — take the same scoped, non-persisted boost VB/WS use. */
  common_emu_auto_oc(1);

  odroid_system_init(APPID_SNES, SNES_AUDIO_RATE);
  odroid_system_emu_init(&snes_LoadState, &snes_SaveState, &snes_Screenshot,
                         NULL, NULL, NULL);

  audio_start_playing(SNES_AUDIO_SAMPLES);

  memset(snes_wram, 0, sizeof(snes_wram));

  uint32_t sz = 0;
  const uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &sz, false);
  if (rom && sz > 512 && (sz % 1024) == 512) {   /* copier header */
    rom += 512;
    sz  -= 512;
  }
  snes_rom = rom; snes_rom_len = sz;

  snes = snes_init(snes_wram);
  g_the_snes = snes;
  /* rc SMW activation takes priority over spin-skip. If the ITCM metadata or
   * ROM identity check fails, fall back to the spin-skip whitelist. */
  if (!rc_smw_activate(rom, sz)) {
    spin_whitelist_set(rom, sz);   /* enable spin-skip only for high-spin ROMs */
  }
  spin_reset();   /* clean slate either way (spin-skip learner / rc dispatch) */

  bool ok = (rom != NULL) && !cart_needs_coprocessor(rom, sz) &&
            snes_loadRom(snes, rom, (int)sz);
  if (!ok) {
    /* No lang_t entry on purpose: strings are positional in the SD language
     * binaries and this core is experimental — English only for now. */
    odroid_overlay_alert("Unsupported SNES cartridge (coprocessor/mapper)");
    odroid_system_switch_app(0);   /* back to launcher */
    return;
  }
  /* The loader (GNW_SNES_CORE build) already points cart->rom straight at the
   * flash-cached image — no malloc'd copy exists to free. Do NOT free cart->rom
   * here: it is read-only flash, not heap, and the header-skip may have offset
   * it past a copier header (snes_rom + 0x200). */

  /* Cheap one-shot boot log — always on (instant, SD idle). The expensive
   * 500-frame DWT profile further below is gated behind SNES_LOAD_DIAG so it
   * never stalls the loading screen.
   * SAFE one-shot probe at LOAD time — before the frame loop,
   * SD idle, never mid-play. Writes straight from the overlay (extflash has room;
   * the resident sd_save_log path would overflow intflash) to /snes_diag.txt.
   * Reports whether the flash-cached ROM reads back sane (internal title +
   * checksum), the #1 suspect for a device-only black screen. Read off the SD. */
  {
    FILE *df = fopen("/snes_diag.txt", "w");
    if (df) {
      const uint8_t *r = snes->cart->rom;
      uint32_t rs = (uint32_t)snes->cart->romSize;
      uint32_t off = (snes->cart->type == 2) ? 0xFFC0u : 0x7FC0u;   /* HiROM : LoROM */
      char title[22];
      for (int i = 0; i < 21; i++) {
        uint8_t c = (r && (off + i) < rs) ? r[off + i] : 0;
        title[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
      }
      title[21] = 0;
      uint32_t sum = 0;
      for (uint32_t i = 0; r && i < rs && i < 0x10000; i++) sum += r[i];
      extern bool g_rc_active;
      fprintf(df, "SNES load: type=%d size=%lu title=[%s] sum64k=%08lX rc=%d\n",
              snes->cart->type, (unsigned long)rs, title, (unsigned long)sum,
              (int)g_rc_active);
      fclose(df);
    }
  }

  /* (Baseline profile, on device, pre-levers: 312 MHz, budget 5.2M cyc/frame;
   * interpreter+APU 7.12M, PPU +2.82M, audio 0.71M → ~29 fps raw. Rig insn ≈
   * device cycle ~1:1.) */

#ifdef SNES_LOAD_DIAG
  /* Headless 500-frame DWT profile (~11-18 s stall). Gated OFF by default;
   * build with -DSNES_LOAD_DIAG (the Makefile's SNES_LOAD_DIAG=1 flag wires
   * it up and swaps in DWT-instrumented apu.c/dsp.c copies that feed the
   * three g_diag_* buckets below). Post-lever frame-cost PROFILE -- same
   * DWT probe, now with spin-skip stats AND an APU split (SPC700 vs DSP,
   * DSP further split into echo FIR vs the rest), so the next lever is
   * aimed at a measured number, not a guess. Headless, before the
   * interactive loop, SD idle, wdog kicked.
   *
   * The g_diag_* counters are defined in the generated apu.c and incremented
   * at every spc_runOpcode / dsp_cycle / dsp_handleEcho call site. They are
   * read ONCE after the 250-frame draw loop and divided by 250 for a per-
   * frame average; DWT_CYCCNT is reset per frame (preserving the existing
   * cyc_skip/cyc_draw measurement), and the per-call deltas the
   * instrumentation accumulates are independent of that reset. */
  {
    extern uint32_t SystemCoreClock;
    extern uint64_t gsnes__g_diag_spc_cycles, gsnes__g_diag_dsp_cycles,
                    gsnes__g_diag_dsp_echo_cycles;
    common_emu_enable_dwt_cycles();
    uint64_t cyc_skip = 0, cyc_draw = 0, cyc_audio = 0;

    for (int i = 0; i < 250; i++) {           /* interpreter+APU only */
      wdog_refresh();
      g_ppu_skip_render = true;
      render_frame_into_active_buffer();
      common_emu_clear_dwt_cycles();
      run_frame_events(snes);
      cyc_skip += common_emu_get_dwt_cycles();
    }
    uint64_t spin_v0 = g_spin.ops_virtual, spin_r0 = g_spin.ops_real;

    /* Zero the APU buckets AFTER the skip loop (which also runs the APU) so
     * the read below captures the draw loop only -- the path the device
     * actually takes when PPU is on and audio is streaming. */
    gsnes__g_diag_spc_cycles = 0;
    gsnes__g_diag_dsp_cycles = 0;
    gsnes__g_diag_dsp_echo_cycles = 0;

    memset(snes_frame, 0, sizeof(snes_frame));
    for (int i = 0; i < 250; i++) {           /* + PPU line renderer + audio */
      wdog_refresh();
      g_ppu_skip_render = false;
      render_frame_into_active_buffer();
      common_emu_clear_dwt_cycles();
      run_frame_events(snes);
      cyc_draw += common_emu_get_dwt_cycles();

      common_emu_clear_dwt_cycles();
      snes_pcm_submit();
      cyc_audio += common_emu_get_dwt_cycles();
    }
    uint64_t vd = g_spin.ops_virtual - spin_v0, rd = g_spin.ops_real - spin_r0;

    /* 250-frame sums -> per-frame averages. skeleton = interpreter+memory
     * alone (cyc_skip minus the APU work that the skip loop also ran);
     * PPU = cyc_draw - cyc_skip (the line renderer + mode 7 the skip loop
     * skipped); DSP non-echo = dsp - echo (the channel mix path a 16 kHz
     * mono decimation would NOT touch); echo = the FIR filter work it
     * WOULD halve. audio = snes_pcm_submit (the DMA buffer handoff). */
    uint64_t skel_pf = cyc_skip / 250 - gsnes__g_diag_spc_cycles / 250
                     - gsnes__g_diag_dsp_cycles / 250;
    uint64_t ppu_pf  = (cyc_draw - cyc_skip) / 250;
    uint64_t spc_pf  = gsnes__g_diag_spc_cycles / 250;
    uint64_t dsp_pf  = gsnes__g_diag_dsp_cycles / 250;
    uint64_t echo_pf = gsnes__g_diag_dsp_echo_cycles / 250;
    uint64_t nonecho_pf = dsp_pf - echo_pf;
    uint64_t audio_pf = cyc_audio / 250;
    uint64_t total_pf = skel_pf + ppu_pf + spc_pf + dsp_pf + audio_pf;

    FILE *df = fopen("/snes_diag.txt", "a");
    if (df) {
      fprintf(df, "SNES profile3: clk=%lu skel=%lu(%lu%%) ppu=%lu(%lu%%) "
                  "spc=%lu(%lu%%) dsp=%lu(%lu%%) [echo=%lu(%lu%%) non_echo=%lu(%lu%%)] "
                  "audio=%lu(%lu%%) total=%lu spin=%lu%%(gate=%d) rc=%d "
                  "hash=%08lX/exp=%08lX\n",
              (unsigned long)SystemCoreClock,
              (unsigned long)skel_pf,  (unsigned long)(total_pf ? 100*skel_pf/total_pf : 0),
              (unsigned long)ppu_pf,   (unsigned long)(total_pf ? 100*ppu_pf/total_pf : 0),
              (unsigned long)spc_pf,   (unsigned long)(total_pf ? 100*spc_pf/total_pf : 0),
              (unsigned long)dsp_pf,   (unsigned long)(total_pf ? 100*dsp_pf/total_pf : 0),
              (unsigned long)echo_pf,  (unsigned long)(dsp_pf ? 100*echo_pf/dsp_pf : 0),
              (unsigned long)nonecho_pf,(unsigned long)(dsp_pf ? 100*nonecho_pf/dsp_pf : 0),
              (unsigned long)audio_pf, (unsigned long)(total_pf ? 100*audio_pf/total_pf : 0),
              (unsigned long)total_pf,
              (unsigned long)((vd + rd) ? (100 * vd / (vd + rd)) : 0),
              (int)g_spin.gate_on,
              (int)g_rc_active,
              (unsigned long)s_rc_diag_crc, (unsigned long)s_rc_diag_exp);
      fclose(df);
    }
  }
#endif /* SNES_LOAD_DIAG */

  if (load_state) {
    odroid_system_emu_load_state(save_slot);
  } else {
    lcd_clear_buffers();
  }
  memset(snes_frame, 0, sizeof(snes_frame));   /* borders start black and stay black */

  while (1) {
    wdog_refresh();

    bool drawFrame = common_emu_frame_loop();

    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, &blit);
    common_emu_input_loop_handle_turbo(&joystick);

    snes->input1->currentState = read_snes_pad(&joystick);

    g_ppu_skip_render = !drawFrame;
    render_frame_into_active_buffer();   /* arm the line callback every frame */
    run_frame_events(snes);

    if (drawFrame) {
      blit();          /* present_frame() + in-game overlay */
      lcd_swap();
    }

    snes_pcm_submit();

    common_emu_sound_sync(false);
  }
}
