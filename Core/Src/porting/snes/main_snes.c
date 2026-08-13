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
#ifdef SNES_SPIN_BAKE
#include "snes/spin_bake.h"
#endif
#include "snes_audio_stretch.h"
#include "snes/rc_dispatch.h"
#include "crc32.h"
#include "snes_profile.h"

bool snes_loadRom(Snes *snes, const uint8_t *data, int length);   /* snes_other.c */

#define SNES_FPS            60
#define SNES_WIDTH          256
#define SNES_HEIGHT         224
/* 16000 by default. The DSP already synthesizes all 534 samples of a frame at
 * 32 kHz and dsp_getSamples box-filters them down to 266, so everything above
 * 8 kHz is produced and then thrown away -- the cost of raising this is the
 * resample/SAI/stretcher work, NOT the synthesis. Overridable so that cost can
 * be measured; note the stretcher's constants below are expressed at 16 kHz and
 * would need scaling before this ships at another rate. */
#ifndef SNES_AUDIO_RATE
#define SNES_AUDIO_RATE     16000
#endif
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

#ifdef SNES_SMW_HLE_PRODUCT
extern int g_wire_on;
void wire_apu_write(Snes *snes, uint32_t adr, uint8_t val);
int wire_try_swap(Snes *snes, int frame);
void wire_frame_audio(int16_t *buf, int n);
bool wire_configure_rom(const uint8_t *rom, uint32_t len);
void wire_prepare_save(void);
void wire_restore_after_load(Snes *snes);
static int smw_hle_frame;
#endif

/* The lib routes $2140-43 writes through this (snes.c). Catch the APU up and
 * write the CPU-visible mailbox — NOT apu_cpuWrite(), which is the SPC's own
 * bus and would leave inPorts stale (boot then spins on the port echo). */
void RtlApuWrite(uint32_t adr, uint8_t val) {
#ifdef SNES_SMW_HLE_PRODUCT
  wire_apu_write(g_the_snes, adr, val);
#else
  snes_catchupApu(g_the_snes);
  g_the_snes->apu->inPorts[adr & 0x3] = val;
#endif
}

void Die(const char *s) {
  printf("SNES Die: %s\n", s);
  assert(!"snes core died");
}
void Warning(const char *s) { (void)s; }

#ifndef SNES_PRESENT_CLEAN_ALL
#define SNES_PRESENT_CLEAN_ALL 0
#endif

extern bool g_ppu_skip_render;   /* ppu.c: skip compositing on dropped frames */
uint32_t g_snes_drawn_frames;    /* frames actually rendered, for draw-rate A/B */
uint32_t g_snes_state_resumed;   /* 1 = the autoboot savestate really loaded */

#ifndef SNES_ABLATE_APU
#define SNES_ABLATE_APU 0
#endif
/* SNES_SPIN_REPLAY_ONLY=1: keep the run_dots replay branch, delete every trace
 * of LEARNING -- the per-opcode note, the pc24/disp it needs, and the bus
 * hooks. The question it answers is the one that decides whether a per-ROM
 * table is worth building at all: if a pattern were known before the ROM
 * starts, discovery is unnecessary, and what remains has to cost less than it
 * saves. Diagnostic: with no learner nothing fills g_spin, so nothing is
 * actually skipped -- this measures the FLOOR the design would pay. */
#ifndef SNES_SPIN_REPLAY_ONLY
#define SNES_SPIN_REPLAY_ONLY 0
#endif
#ifndef SNES_ABLATE_CPU
#define SNES_ABLATE_CPU 0
#endif
#ifndef SNES_ABLATE_DSP
#define SNES_ABLATE_DSP 0
#endif

/* ---- state ---------------------------------------------------------------- */
static Snes *snes;
/* 128 KB WRAM lives in the overlay BSS (like the SM port's g_ram): the AHB
 * pool is only 120 KB total and the Apu (~66 KB) already comes from it. */
static uint8_t snes_wram[0x20000];
static int16_t audio_buf[SNES_AUDIO_SAMPLES];  /* mono frame mix from the DSP  */

#if SNES_FRAME_HIST
/* Read out over SWD, so the names must survive -- deliberately NOT static, and
 * in the overlay's own namespace via snes_redefines. One bucket per 2^SHIFT
 * core cycles; at 340 MHz an audio period (16.625 ms) is 5.65 M cycles, which
 * is bucket 21 at SHIFT=18. The bucket the period line falls in is what the
 * tool prints a marker on. */
#define SNES_FH_SHIFT   18
#define SNES_FH_BUCKETS 48
uint32_t snes_fh_bucket[SNES_FH_BUCKETS];
uint32_t snes_fh_last, snes_fh_max, snes_fh_n;
uint64_t snes_fh_sum;
/* A slow frame is only actionable once you know which half of it is slow, so
 * the frame is split at the one boundary that matters: emulation (the event
 * loop) versus everything the firmware does around it (present, audio, swap).
 * Summed separately for frames that crossed the period line and frames that did
 * not -- the difference between those two averages IS the thing to fix. */
uint32_t snes_fh_mark;            /* DWT at the emu/present boundary */
uint64_t snes_fh_emu_over, snes_fh_rest_over;
uint64_t snes_fh_emu_under, snes_fh_rest_under;
uint32_t snes_fh_n_over, snes_fh_n_under;
uint32_t snes_fh_skipped_over;    /* of those, how many were frameskipped */
static inline uint32_t snes_frame_hist_now(void) { return common_emu_get_dwt_cycles(); }
#endif

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
#if defined(SNES_SPIN_SKIP) && !SNES_SPIN_REPLAY_ONLY
  const bool learn = spin_engaged();   /* sample before the opcode; see header */
  uint32_t pc24 = 0;
  int disp = 0;
  if (learn) {
    pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
    disp = (cpu->nmiWanted || (cpu->irqWanted && !cpu->i) || cpu->waiting) && !cpu->stopped;
  }
#endif
  s->cpuMemOps = 0;
#if SNES_ABLATE_CPU
  /* ABLATION, WRONG OUTPUT ON PURPOSE. The 65816 never executes: the scheduler
   * is charged a plausible opcode and moves on, so the PPU, the APU, the DMA
   * and the frame loop all keep running against a machine whose CPU is frozen.
   * The screen holds whatever the last real frame left; the frame counter is
   * the only valid reading.
   *
   * This is the one block on the board that has never had a number. The issue
   * names the interpreter as one of the two places the next fps can come from,
   * and "no lever" was only ever a statement about C fallbacks (there are zero)
   * -- not an answer to "what is the frame worth if the interpreter were free".
   * 6 cycles is the common short opcode; the exact charge only sets how many
   * opcodes a frame contains, and any plausible value answers the question. */
  (void)cpu;
  int cycles = 6;
#else
  int cycles = SNES_PROF_CPU_CALL(CPU_RUN_OPCODE(cpu));
#endif
  /* 6*cycles + 2*memOps -- identical to the old 8-per-access charge plus
   * (cycles - memOps)*6, but paid once instead of on every bus access. */
  s->cpuCyclesLeft += cycles * 6 + s->cpuMemOps * 2;
#if defined(SNES_SPIN_SKIP) && !SNES_SPIN_REPLAY_ONLY
  if (learn) SNES_PROF_SPIN_CALL(spin_note_real(cpu, pc24, (uint8_t)s->cpuCyclesLeft, disp));
#endif
  return cycles;
}

static void cpu_tick(Snes *s) {
  if (dma_cycle(s->dma)) return;
  if (s->cpuCyclesLeft == 0) run_one_opcode(s);
  s->cpuCyclesLeft -= 2;
}

static void run_dots(Snes *s, int dots) {
  Cpu *cpu = s->cpu;
#ifdef SNES_SPIN_BAKE
  /* One test per SPAN, outside the loop below, which is therefore compiled
   * exactly as it was before this feature existed. See spin_bake.h. */
  if ((uint16_t)(s->cpu->pc - g_bake.pc_load) <= 2u)
    dots = spin_bake_run_span(s, s->cpu, dots);
#endif
  bool dma_active = s->dma->dmaBusy || s->dma->hdmaTimer > 0;
  while (dots > 0) {
    if (dma_active) {
      dma_cycle(s->dma);
      s->apuDotsAccum += 2;
      s->hPos += 2; dots -= 2;
      dma_active = s->dma->dmaBusy || s->dma->hdmaTimer > 0;
      continue;
    }
    bool started_dma = false;
    if (s->cpuCyclesLeft == 0) {
      /* Replay branch: a learned pure wait-loop iteration is a no-op — charge
       * the recorded cycle pattern and park the pc where the real call would
       * have, WITHOUT the interpreter. Falls through to the shared bulk-consume
       * so hPos steps and the apuCatchupCycles FMA sequence stay bit-identical. */
#ifdef SNES_SPIN_SKIP
      /* Compiled out with the learner: spin_note() is its only writer of
       * g_spin.on and that is SNES_SPIN_SKIP-only, so with the flag off this
       * was a load and a branch per opcode to test a condition that could not
       * be true. The learner's other taxes were removed in 559d9970; this one
       * was left because it looked free. Nothing per-opcode is free here. */
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
      } else
#endif
      {
        apply_irq_match(s);
        run_one_opcode(s);
        started_dma = s->dma->dmaBusy || s->dma->hdmaTimer > 0;
      }
    }
    int step;
    if (s->cpuCyclesLeft >= 2 && !started_dma) {
      step = s->cpuCyclesLeft;
      if (step > dots) step = dots;
      s->cpuCyclesLeft -= (uint8_t)step;
    } else {
      step = 2;
      s->cpuCyclesLeft -= 2;
    }
    s->apuDotsAccum += step;
    s->hPos += step; dots -= step;
    dma_active = started_dma;
  }
}

/* The armed test lives HERE, not in run_dots. One level down it made run_dots a
 * real call -- it had been inlined into its caller -- and that alone cost 2.13%
 * of a frame on Super Mario Kart with the guard folded away and nothing
 * installed. One branch per frame is free; one per span is not. */
static void run_frame_events(Snes *s) {
  for (;;) {
    s->apuDotsAccum += 2;
    snes_handle_pos_stuff(s);
    cpu_tick(s);
    if (s->hPos == 0 && s->vPos == 0) break;
    run_dots(s, dots_to_next_event(s));
  }
  snes_catchupApu(s);
  spin_frame_tick();   /* auto-gate: park the learner on non-spinning carts */
#ifdef SNES_SPIN_BAKE
  spin_bake_frame_tick();   /* the bake's own gate -- once a frame, never per opcode */
#endif
#ifdef SNES_SMW_HLE_PRODUCT
  wire_try_swap(s, smw_hle_frame++);
#endif
}

#ifdef SNES_SPIN_BAKE

#endif

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
 * Render into a PRIVATE persistent framebuffer, then copy the complete visible
 * 320x240 image to whichever LCD buffer is active.  This keeps both LCD buffers
 * complete across swaps/overlays.  SNES_DIRECT_VIDEO lets the PPU write its RGB565
 * scanlines directly into that private framebuffer, removing the old 512-byte
 * scratch-to-frame memcpy on every visible line. */
#define SNES_TOP_MARGIN  ((GW_LCD_HEIGHT - SNES_HEIGHT) / 2)   /* (240-224)/2 = 8 */
#define SNES_LEFT_MARGIN ((GW_LCD_WIDTH - SNES_WIDTH) / 2)     /* (320-256)/2 = 32 */
#ifdef SNES_DIRECT_VIDEO
/* Overscan can produce 240 lines.  Starting at row 8 therefore needs eight
 * hidden tail rows; present_frame still copies only the visible 240 rows. */
#define SNES_FRAME_ROWS (GW_LCD_HEIGHT + SNES_TOP_MARGIN)
#else
#define SNES_FRAME_ROWS GW_LCD_HEIGHT
static uint16_t snes_line[256];
#endif
static uint16_t snes_frame[GW_LCD_WIDTH * SNES_FRAME_ROWS];

#ifdef SNES_LOAD_DIAG
/* DWT profiling accumulators for the SNES_LOAD_DIAG block further below.
 * Defined here rather than in the generated apu.c (where they used to live)
 * because SNES_APU_SOURCE replaces that file with the audio HLE's apu_wire.c
 * whenever SNES_SMW_HLE=1 / SNES_NSPC_HLE=1 -- so the definitions dropped out
 * of the link precisely when HLE was enabled, and SNES_LOAD_DIAG=1 could not
 * be combined with it. That combination is the interesting one (HLE is what
 * ships), and it was never profiled on device for exactly this reason.
 * main_snes.c always compiles, so the counters always exist; the instrumented
 * apu.c/dsp.c reach them as externs through the forced -include of
 * snes_diag_accum.h. With HLE active nothing increments the spc counter --
 * a 0 there is the correct reading, not a missing probe: the SPC700
 * interpreter genuinely no longer runs. */
uint64_t gsnes__g_diag_spc_cycles = 0;
uint64_t gsnes__g_diag_dsp_cycles = 0;
uint64_t gsnes__g_diag_dsp_echo_cycles = 0;
#endif

#ifndef SNES_DIRECT_VIDEO
static void snes_blit_line(unsigned y, const uint16_t *line) {
  if (y < 1) return;   /* y is 1-based */
  unsigned row = (y - 1) + SNES_TOP_MARGIN;
  if (row >= GW_LCD_HEIGHT) return;   /* clip overscan past the panel */
  memcpy(snes_frame + row * GW_LCD_WIDTH + SNES_LEFT_MARGIN, line, sizeof(snes_line));
}
#endif

#ifndef SNES_ABLATE_FB
#define SNES_ABLATE_FB 0
#endif
#if SNES_ABLATE_FB
/* The scratch line the ablation renders every scanline into. It exists outside
 * the SNES_DIRECT_VIDEO branch that normally owns snes_line, because this build
 * takes neither path. */
static uint16_t snes_ablate_line[256 + 2 * 8];
#endif
static void render_frame_into_active_buffer(void) {
#if SNES_ABLATE_FB
  /* ABLATION, WRONG OUTPUT ON PURPOSE. Point the PPU at the 512-byte scratch
   * line with pitch 0, so every scanline lands on the same address. All the VRAM
   * reads, the decode and the compositing still happen -- only the 158,720-byte
   * framebuffer's footprint disappears.
   *
   * The question it answers: the D-cache is 16 KB and this build writes a 155 KB
   * framebuffer through it every drawn frame. Every one of those writes
   * allocates a line and evicts something, and the only thing measured as
   * expensive on this part is reading a COLD line out of the 64 KB of VRAM. If
   * the framebuffer is what keeps VRAM cold, this ablation shows it and the fix
   * is an MPU region, not a rewrite. If it shows nothing, VRAM misses are
   * inherent and that avenue closes.
   *
   * Nothing reaches the LCD in this mode; the frame counter is the only reading. */
  g_ppu_line_cb = NULL;
  PpuBeginDrawing(snes->ppu, (uint8_t *)snes_ablate_line, 0, 0);
  return;
#endif
#ifdef SNES_DIRECT_VIDEO
  g_ppu_line_cb = NULL;
  PpuBeginDrawing(snes->ppu,
                  (uint8_t *)(snes_frame + SNES_TOP_MARGIN * GW_LCD_WIDTH +
                              SNES_LEFT_MARGIN),
                  GW_LCD_WIDTH * sizeof(uint16_t), 0);
#else
  g_ppu_line_cb = &snes_blit_line;
  PpuBeginDrawing(snes->ppu, (uint8_t *)snes_line, 0, 0);  /* pitch 0: every line here */
#endif
}

/* DMA2D M2M offload for the OFF-scaling copy below. snes_frame -> LCD active
 * buffer is a flat 320x240 RGB565 copy with no pixel-format conversion or
 * scaling, the textbook DMA2D_M2M case.
 *
 * DMA2D is a single shared peripheral — hw_jpeg_decoder.c also drives it
 * (cover art, YCbCr M2M_BLEND, non-zero InputOffset) through its own,
 * separate DMA2D_HandleTypeDef. Two handles, same hardware CR/OPFCCR/OOR/FGOR
 * registers underneath: if that code runs between two of our frames (e.g. a
 * savestate thumbnail while paused) and we only configured once at startup,
 * our next HAL_DMA2D_Start would fire with its leftover Mode/offset instead
 * of ours — wrong stride, garbage on screen. So reconfigure (Init +
 * foreground ConfigLayer, both idempotent register writes) every call
 * instead of caching a "ready" flag; the cost is a handful of register
 * writes against a ~150KB transfer, noise either way. */
static DMA2D_HandleTypeDef snes_dma2d;
static bool snes_dma2d_pending;

static bool snes_dma2d_configure(void) {
  snes_dma2d.Instance = DMA2D;
  snes_dma2d.Init.Mode = DMA2D_M2M;
  snes_dma2d.Init.ColorMode = DMA2D_OUTPUT_RGB565;
  snes_dma2d.Init.OutputOffset = 0;
  snes_dma2d.Init.AlphaInverted = DMA2D_REGULAR_ALPHA;
  snes_dma2d.Init.RedBlueSwap = DMA2D_RB_REGULAR;
  snes_dma2d.Init.BytesSwap = DMA2D_BYTES_REGULAR;
  snes_dma2d.Init.LineOffsetMode = DMA2D_LOM_PIXELS;
  if (HAL_DMA2D_Init(&snes_dma2d) != HAL_OK)
    return false;

  /* Foreground (source) layer: RGB565, zero offset — snes_frame's rows are
   * tightly packed at exactly GW_LCD_WIDTH pixels, matching source stride. */
  snes_dma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_RGB565;
  snes_dma2d.LayerCfg[1].InputOffset = 0;
  snes_dma2d.LayerCfg[1].AlphaMode = DMA2D_NO_MODIF_ALPHA;
  snes_dma2d.LayerCfg[1].InputAlpha = 0xFF;
  snes_dma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
  snes_dma2d.LayerCfg[1].RedBlueSwap = DMA2D_RB_REGULAR;
  return HAL_DMA2D_ConfigLayer(&snes_dma2d, 1) == HAL_OK;
}

/* Block until a DMA2D copy started by present_frame() has landed. Must run
 * before anything else touches the destination buffer (overlay draw,
 * lcd_swap, or a later present_frame call) — DMA2D is cache-blind and races
 * with the CPU exactly like any other bus master would. No-op when the OFF
 * path didn't use DMA2D (fallback, or a scaling mode change this frame). */
static void present_frame_wait(void) {
  if (!snes_dma2d_pending) return;
  /* Poll in slices, refreshing the watchdog between them, instead of one
   * blocking 100 ms call. odroid_overlay_dialog() refreshes once per menu-loop
   * iteration, but that is OUTSIDE this function -- the whole wait happens
   * inside a single _repaint() call, so a long transfer spends all of it with
   * the watchdog unfed. Slicing keeps the same worst-case wait while never
   * leaving the watchdog starved for more than one slice.
   * HAL_DMA2D_PollForTransfer returns HAL_TIMEOUT without disturbing the
   * transfer, so re-entering it simply continues waiting. */
  for (int i = 0; i < 10; i++) {
    wdog_refresh();
    if (HAL_DMA2D_PollForTransfer(&snes_dma2d, 10) != HAL_TIMEOUT)
      break;
  }
  wdog_refresh();
  snes_dma2d_pending = false;
}

static void present_frame(void) {
  uint16_t *dst = lcd_get_active_buffer();
  odroid_display_scaling_t scaling = odroid_display_get_scaling_mode();

  if (scaling == ODROID_DISPLAY_SCALING_OFF) {
    /* 1:1 centred (256x224 in 320x240): snes_frame already has black borders
     * baked in (cleared once at init). DMA2D moves the 320x240 straight copy
     * into the background so the CPU can spend that time on audio/pacing
     * instead of blocked on an uncached memcpy — the caller MUST reach
     * present_frame_wait() before touching dst again. */
#ifdef SNES_PRESENT_DMA2D
    if (snes_dma2d_configure()) {
      /* snes_frame lives in cacheable RAM_EMU; SNES_DIRECT_VIDEO's PPU write
       * this frame's pixels via normal cached stores. DMA2D is a bus master
       * and cache-blind, so any dirty line not yet evicted would be read as
       * stale — clean (not invalidate: the next frame's PPU render is about
       * to overwrite this same buffer through the cache again) before
       * handing the address to hardware. */
      /* SNES_PRESENT_CLEAN_ALL=1 cleans the whole D-cache instead of walking
       * the buffer by address. The buffer is 158,720 B = 4,960 cache lines; the
       * D-cache holds 512. So the by-address form issues ten maintenance ops
       * for every line that could possibly be dirty, and most of them name an
       * address the cache has never held. Cleaning by set/way touches 512
       * entries and is a strict superset. Whether the extra write-back of other
       * subsystems' dirty lines costs more than the 4,448 wasted DCCMVACs is a
       * question for the device, and the device answered: 52.17 fps against
       * 52.36, and with three times the run-to-run spread. The write-back of
       * everything else costs more than the wasted maintenance ops save. OFF. */
#if SNES_PRESENT_CLEAN_ALL
      SCB_CleanDCache();
#else
      SCB_CleanDCache_by_Addr((uint32_t *)snes_frame, sizeof(snes_frame));
#endif
      HAL_DMA2D_Start(&snes_dma2d, (uint32_t)snes_frame, (uint32_t)dst,
                       GW_LCD_WIDTH, GW_LCD_HEIGHT);
      snes_dma2d_pending = true;
      return;
    }
    /* DMA2D unavailable for some reason — fall back to the plain CPU copy. */
#endif
    /* Revert switch: drop -DSNES_PRESENT_DMA2D in Makefile.common's SNES
     * recipe to force this path unconditionally (present_frame_wait() stays
     * a safe no-op either way, snes_dma2d_pending never becomes true). */
    memcpy(dst, snes_frame, GW_LCD_WIDTH * GW_LCD_HEIGHT * sizeof(uint16_t));
    return;
  }

  if (scaling == ODROID_DISPLAY_SCALING_FULL) {
    /* Stretch 256x224 → 320x240 (fills the panel, slight aspect distortion).
     * Nearest-neighbour: for each dest pixel, pick the closest source pixel. */
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
      int sy = (y * SNES_HEIGHT) / GW_LCD_HEIGHT;
      const uint16_t *srow = snes_frame + (sy + SNES_TOP_MARGIN) * GW_LCD_WIDTH + SNES_LEFT_MARGIN;
      uint16_t *drow = dst + y * GW_LCD_WIDTH;
      for (int x = 0; x < GW_LCD_WIDTH; x++)
        drow[x] = srow[(x * SNES_WIDTH) / GW_LCD_WIDTH];
    }
    return;
  }

  /* FIT (and CUSTOM, treated the same): aspect-preserving. SNES 256:224 =
   * 8:7 ≈ 1.143. The LCD 320:240 = 4:3 ≈ 1.333 is wider, so fill the height
   * (240) and letterbox the width: fit_w = 240 * 256/224 ≈ 274, side borders
   * ≈ 23 px each. */
  {
    int fit_w = (SNES_WIDTH * GW_LCD_HEIGHT + SNES_HEIGHT / 2) / SNES_HEIGHT;
    int lpad = (GW_LCD_WIDTH - fit_w) / 2;
    for (int y = 0; y < GW_LCD_HEIGHT; y++) {
      int sy = (y * SNES_HEIGHT) / GW_LCD_HEIGHT;
      const uint16_t *srow = snes_frame + (sy + SNES_TOP_MARGIN) * GW_LCD_WIDTH + SNES_LEFT_MARGIN;
      uint16_t *drow = dst + y * GW_LCD_WIDTH;
      for (int x = 0; x < lpad; x++) drow[x] = 0;          /* left border */
      for (int x = 0; x < fit_w; x++)
        drow[lpad + x] = srow[(x * SNES_WIDTH) / fit_w];   /* scaled image */
      for (int x = lpad + fit_w; x < GW_LCD_WIDTH; x++) drow[x] = 0;  /* right */
    }
  }
}

/* Present the last rendered frame and draw the in-game overlay on top. Used both
 * as the normal per-frame present and as the overlay's repaint callback, so the
 * pause menu keeps the game behind it instead of a stale/black background.
 * Fully synchronous (waits out any DMA2D copy immediately) — callers that want
 * the async overlap split present_frame()/present_frame_wait() themselves; see
 * the main loop below. */
static void blit(void) {
  present_frame();
  present_frame_wait();
  common_ingame_overlay();
}

/* ---- audio ----------------------------------------------------------------
 * Top the DSP up to one frame of samples (534 stereo pairs internally) and
 * downmix to 16 kHz mono, exactly like the harness/rig. */

static void snes_pcm_submit(void) {
  if (snes->apu) {
#ifdef SNES_SMW_HLE_PRODUCT
    if (g_wire_on) {
      wire_frame_audio(audio_buf, SNES_AUDIO_SAMPLES);
    } else
#endif
    {
    /* Ledger B, APU-exclusive scope: this whole block is what an exact wire
     * replaces -- the SPC700/DSP top-up AND the sample extraction. Bracketing
     * only the apu_cycle loop would undercount the recoverable cost. One scope
     * per frame, so the probe is noise. */
    uint32_t apu_t0 = SNES_PROF_APU_SCOPE_ENTER();
#if !(SNES_ABLATE_APU || SNES_ABLATE_DSP)
    while (snes->apu->dsp->sampleOffset < 534)
      apu_cycle(snes->apu);
#else
    /* ABLATION ONLY. This loop drains the DSP by running the APU until it has
     * emitted a frame of samples, so it is coupled to the very thing an
     * ablation deletes: with the DSP gone nothing advances sampleOffset and the
     * loop never ends. Faking the counter inside apu_cycle does not fix it
     * either -- the SPC still runs, so the loop then executes 17,088 apu_cycle
     * calls a frame and the DSP prices as COSTING 1.5 fps to remove. Stop the
     * loop where it is written instead. */
#endif
    dsp_getSamples(snes->apu->dsp, audio_buf, SNES_AUDIO_SAMPLES, 1);
    SNES_PROF_APU_SCOPE_EXIT(apu_t0);
    }
  } else {
    memset(audio_buf, 0, sizeof(audio_buf));
  }

  /* Hand the frame's emulated samples to the stretcher instead of straight to
   * the DMA. Below 60 emulated fps the DMA eats more buffers than the core
   * fills, and the old code's answer was to write 266 samples and zero the
   * rest of the buffer -- an audible gap every slow frame. See
   * snes_audio_stretch.h; the emulated machine is not touched, only the rate
   * the already-produced samples are played back at. */
  snes_stretch_push(audio_buf, SNES_AUDIO_SAMPLES);
  /* No emit here. The SAI ISR pulls one half-buffer per DMA period through
   * snes_audio_isr_pull(), so "exactly one fill per period" is a property of
   * the hardware's own callback rather than a rule the frame loop has to
   * remember -- and forgetting it is precisely what broke the sound: dropping
   * the pacing wait left the catch-up loop emitting two or three times inside
   * one period, draining the stretcher ring that many times faster than the
   * DMA consumed it, so what played was filler instead of the game. */
}

/* Called from the SAI ISR, once per DMA half-buffer. Keep it to what the old
 * emit did -- pull, mute, volume -- and nothing that can block: this runs at
 * interrupt priority. */
static void snes_audio_isr_pull(int16_t *dst, uint16_t n) {
  if (common_emu_sound_loop_is_muted()) {
    memset(dst, 0, (size_t)n * sizeof(int16_t));
    return;
  }
  snes_stretch_pull(dst, n);
  int32_t factor = common_emu_sound_get_volume();
  for (uint16_t i = 0; i < n; i++)
    dst[i] = (int16_t)(((int32_t)dst[i] * factor) >> 8);
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
#ifdef SNES_SMW_HLE_PRODUCT
  wire_prepare_save();
#endif
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

/* Why the last state was refused, readable over SWD.
 *
 * The refusal itself is right, but it said only "refused" -- and this tree's own
 * rule is that a red gate must name what failed. Diagnosing one SMW state by
 * hand today cost two wrong guesses (wrong build; wrong SRAM size) before the
 * file turned out to be well-formed, current-version and self-consistent. The
 * numbers that decide it are three words long; publish them.
 *
 *   [0] stage: 0 = ok, 1 = fopen, 2 = short header, 3 = magic, 4 = version,
 *              5 = payload length, 6 = file size, 7 = short read
 *   [1] what the file said   [2] what this build expected   [3] file size
 */
uint32_t g_snes_state_refuse[4];

static bool snes_LoadState(const char *pathName) {
  g_snes_state_refuse[0] = 0;
  FILE *f = fopen(pathName, "rb");
  if (!f) { g_snes_state_refuse[0] = 1; return false; }
  snes_state_header_t h;
  if (fread(&h, 1, sizeof(h), f) != sizeof(h)) {
    g_snes_state_refuse[0] = 2;
    fclose(f);
    return false;
  }
  if (h.magic != SNES_STATE_MAGIC || h.version != SNES_STATE_VERSION) {
    /* Not ours / other build: refuse rather than restore nonsense. */
    g_snes_state_refuse[0] = (h.magic != SNES_STATE_MAGIC) ? 3u : 4u;
    g_snes_state_refuse[1] = (h.magic != SNES_STATE_MAGIC) ? h.magic : h.version;
    g_snes_state_refuse[2] = (h.magic != SNES_STATE_MAGIC) ? SNES_STATE_MAGIC
                                                           : SNES_STATE_VERSION;
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
    g_snes_state_refuse[0] = (h.length != expected) ? 5u : 6u;
    g_snes_state_refuse[1] = h.length;
    g_snes_state_refuse[2] = expected;
    g_snes_state_refuse[3] = (uint32_t)fsize;
    printf("snes: state refused, payload %lu but this build streams %lu "
           "(file %lu bytes)\n", (unsigned long)h.length,
           (unsigned long)expected, (unsigned long)fsize);
    fclose(f);
    return false;
  }
  fseek(f, sizeof(h), SEEK_SET);
  state_file = f; state_bytes = 0;
  state_stream(&state_read);
  fclose(f);
  state_file = NULL;
  if (state_bytes != h.length) {
    g_snes_state_refuse[0] = 7u;
    g_snes_state_refuse[1] = h.length;
    g_snes_state_refuse[2] = state_bytes;
  }
  lcd_clear_active_buffer();
  /* A load replaces the whole machine: a learned spin pattern (and its purity
   * sequence history) now describes a machine that no longer exists. Relearn. */
  spin_reset();
#ifdef SNES_SMW_HLE_PRODUCT
  wire_restore_after_load(snes);
#endif
  return state_bytes == h.length;
}

static void *snes_Screenshot(void) {
  lcd_wait_for_vblank();
  return lcd_get_active_buffer();
}

#ifdef SNES_SMW_HLE_PRODUCT
/* One-shot, quit-time-only flush of the audio-HLE swap status. Registered
 * below as BOTH sram_save_cb (fires from odroid_system_switch_app, i.e. the
 * in-game pause menu's Quit/Save&Quit) and shutdown_cb (fires from
 * odroid_system_shutdown, i.e. power-off/standby) -- SNES has no cart-SRAM
 * handler of its own to conflict with, and both call sites are single,
 * quit-time events, never the frame loop (rule-no-sd-write-during-play).
 * The load-time probe further below can only ever say the gate was ARMED
 * (wire_try_swap() needs ~180+ live frames it doesn't have yet at load) --
 * this is what actually answers whether the swap happened, and if not, the
 * concrete reason, without needing a second device round-trip to find out.
 * Appends a second line to the same /snes_diag.txt so one file covers both
 * halves. */
static void snes_wire_diag_flush(void) {
  extern int g_wire_enable, g_wire_on;
  FILE *df = fopen("/snes_diag.txt", "a");
  if (!df) return;
  /* g_wire_attempt_count/g_wire_swap_frame/g_wire_last_fail_reason are
   * smw_exact_wire.c-specific (its own per-attempt diagnostics) -- they
   * only exist when SMW's exact backend is actually compiled
   * (SNES_SMW_HLE_PRESENT). A generic-only build (SNES_NSPC_HLE=1,
   * SNES_SMW_HLE=0) has neither smw_exact_wire.c nor those globals; an
   * unguarded extern here would be a link error in that configuration. */
#ifdef SNES_SMW_HLE_PRESENT
  extern int g_wire_attempt_count, g_wire_swap_frame;
  extern const char *g_wire_last_fail_reason;
  fprintf(df, "SNES wire at quit: g_wire_enable=%d g_wire_on=%d attempts=%d "
              "swap_frame=%d last_fail=[%s]\n",
          g_wire_enable, g_wire_on, g_wire_attempt_count, g_wire_swap_frame,
          g_wire_last_fail_reason);
#else
  fprintf(df, "SNES wire at quit: g_wire_enable=%d g_wire_on=%d "
              "(generic-only build, no SMW-exact attempt/fail diagnostics)\n",
          g_wire_enable, g_wire_on);
#endif
  fclose(df);
}
#endif

/* ---- ROM loading -----------------------------------------------------------
 * The cart stays memory-mapped in external flash (flash-cache machinery); the
 * core only reads it. Copier headers (512 bytes) are skipped in place. */
static const uint8_t *snes_rom;
static uint32_t snes_rom_len;

/* $ffd6 (ROM type): 0=ROM 1=ROM+RAM 2=ROM+RAM+battery; 3+ = coprocessor
 * (DSP-x/SA-1/SuperFX/...). The DSP-1 family (high nibble 0, low nibble 3-5)
 * has HLE support in dsp1_hle.c — those are allowed through. Every other
 * coprocessor (SuperFX/SA-1/Cx4/S-DD1/...) is still rejected. Find the header
 * the same way the loader scores it: the offset whose checksum ^ complement is
 * 0xFFFF wins; if neither validates, let snes_loadRom decide. */
static bool cart_needs_coprocessor(const uint8_t *rom, uint32_t len) {
  static const uint32_t offs[2] = { 0x7fb0, 0xffb0 };   /* LoROM, HiROM */
  for (int i = 0; i < 2; i++) {
    if (offs[i] + 0x30 > len) continue;
    const uint8_t *h = rom + offs[i];
    uint16_t cks  = h[0x2e] | (h[0x2f] << 8);
    uint16_t icks = h[0x2c] | (h[0x2d] << 8);
    if ((cks ^ icks) == 0xffff) {
      uint8_t romType = h[0x26];   /* $ffd6 = header+0x26 */
      /* high nibble 0 = DSP family (HLE in dsp1_hle.c); anything else with
       * romType >= 3 is a coprocessor we don't support yet */
      if (romType >= 0x03 && (romType >> 4) != 0)
        return true;
    }
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

/* Which artefact the deficit becomes, chosen by the person listening.
 *
 * Below 60 emulated fps the core produces fewer samples than the DAC consumes --
 * 5% short at 57 -- and that has to go somewhere. Splicing keeps the pitch and
 * is inaudible on tone, but on noise (Zelda 3's rain) it is the crackle;
 * following the rate never splices anything but transposes everything by the
 * same 5%. Measured over 1800 frames of that scene: 184 splices and 684
 * dropouts against 0 and 68.
 *
 * There is no useful middle -- a floor that stops short leaves a remainder and
 * the remainder is spliced exactly as before -- so this is a switch, and it is
 * here rather than in the makefile because the answer is a preference. Not
 * persisted: it costs nothing to set and adding a field to persistent_config_t
 * would reset every user's settings. */
static bool snes_submenu_audio_mode(odroid_dialog_choice_t *option,
                                    odroid_dialog_event_t event, uint32_t repeat)
{
  (void)repeat;
  if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT)
    g_snes_audio_gapfree = !g_snes_audio_gapfree;
  strcpy(option->value, g_snes_audio_gapfree ? "Gap-free" : "Pitch");
  return event == ODROID_DIALOG_ENTER;
}

/* In ITCM with the engine it drives. This is the frame loop, and its inner part
 * calls cpu_runOpcode and snes_cpuRead once per opcode -- both now in ITCM at
 * 0x00000000 while this sits in the overlay at 0x24000000, past BL's +-16 MB, so
 * each of those calls went through a linker veneer. Moving the two callees was
 * worth +1.5 fps on the device; this is the caller side of the same boundary. */
#ifdef SNES_BUS_IN_ITCM
__attribute__((section(".itcm_snes_interp.thumb2.bus")))
#endif
void app_main_snes(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
  /* Level-1 OC (312 MHz) to defend the SNES framerate — never downclock a
   * user who chose higher (level 2 = 340). SNES is heavy; stock 280 loses
   * frames the audio HLE just bought back. */
  extern void SystemClock_Config(uint8_t oc_level);
  extern uint8_t odroid_settings_cpu_oc_level_get(void);
  if (odroid_settings_cpu_oc_level_get() < 1) SystemClock_Config(1);

  odroid_gamepad_state_t joystick;
  static char audio_mode_value[16];
  odroid_dialog_choice_t options[] = {
      {400, "Audio", audio_mode_value, 1, &snes_submenu_audio_mode},
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
#ifdef SNES_SMW_HLE_PRODUCT
  odroid_system_emu_init(&snes_LoadState, &snes_SaveState, &snes_Screenshot,
                         &snes_wire_diag_flush, NULL, &snes_wire_diag_flush, NULL);
#else
  odroid_system_emu_init(&snes_LoadState, &snes_SaveState, &snes_Screenshot,
                         NULL, NULL, NULL, NULL);
#endif

  audio_start_playing(SNES_AUDIO_SAMPLES);
  /* After, not before: audio_start_playing clears emu_owns for exactly this
   * reason -- every overlay links at the same address, so a pointer left
   * registered by the previous core would point into this one's data. */
  emu_audio_register(snes_audio_isr_pull);
  emu_audio_enable(1);

  memset(snes_wram, 0, sizeof(snes_wram));

  uint32_t sz = 0;
  const uint8_t *rom = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &sz, false);
  if (rom && sz > 512 && (sz % 1024) == 512) {   /* copier header */
    rom += 512;
    sz  -= 512;
  }
  snes_rom = rom; snes_rom_len = sz;

#ifdef SNES_SMW_HLE_PRODUCT
  smw_hle_frame = 0;
  bool wire_armed = wire_configure_rom(rom, sz);
  printf("SNES SMW audio HLE: %s\n", wire_armed ? "armed (exact ROM)" : "LLE fallback");
#endif

  snes = snes_init(snes_wram);
  g_the_snes = snes;
  /* rc SMW activation takes priority over spin-skip. If the ITCM metadata or
   * ROM identity check fails, fall back to the spin-skip whitelist. */
  if (!rc_smw_activate(rom, sz)) {
    spin_whitelist_set(rom, sz);   /* enable spin-skip only for high-spin ROMs */
  }
#ifdef SNES_SPIN_FORCE_ON
  /* DIAGNOSTIC ARM. spin_table turns Zelda 3 off by name on the strength of a
   * 25.0% skip rate against a ~50% break-even -- and BOTH of those numbers are
   * QEMU rig instruction counts. This tree's own ledger says that rig is blind
   * to roughly 70% of hardware time, and a replayed opcode skips the cache
   * misses and flash stalls as well as the interpreter work, so the DEVICE
   * break-even should sit below the rig's. Nobody has measured it. This forces
   * the learner on regardless of the table so the third arm of a three-way
   * device A/B exists at all; it is not a shipping path. */
  extern bool g_spin_whitelist;
  g_spin_whitelist = true;
#endif
  spin_reset();   /* clean slate either way (spin-skip learner / rc dispatch) */
  snes_stretch_reset();   /* and the audio stretcher: a new ROM starts empty */

  bool ok = (rom != NULL) && !cart_needs_coprocessor(rom, sz) &&
            snes_loadRom(snes, rom, (int)sz);
  if (!ok) {
    /* No lang_t entry on purpose: strings are positional in the SD language
     * binaries and this core is experimental — English only for now. */
    odroid_overlay_alert("Unsupported SNES cartridge (coprocessor/mapper)");
    odroid_system_switch_app(0);   /* back to launcher */
    return;
  }
#ifdef SNES_SPIN_BAKE
  /* After the load: the scan maps a ROM offset to a pc through cart->type,
   * which is what snes_loadRom decides. */
  spin_bake_scan(snes);
  printf("SNES bake: on=%d sites=%lu pc=%02x:%04x dp=$%02x\n", (int)g_bake.on,
         (unsigned long)g_bake.sites, g_bake.bank, g_bake.pc_load, g_bake.dp_off);
#endif
  /* The loader (GNW_SNES_CORE build) already points cart->rom straight at the
   * flash-cached image — no malloc'd copy exists to free. Do NOT free cart->rom
   * here: it is read-only flash, not heap, and the header-skip may have offset
   * it past a copier header (snes_rom + 0x200). */

  /* The load-time /snes_diag.txt probe lived here: ROM title, 64K checksum,
   * dsp1/ramSize and the HLE wire flags, written on every ROM load. It was
   * bring-up instrumentation for the device-only black screen and the Mario
   * Kart BSOD, both of which are closed, and it wrote a file to the card on
   * every single launch to say so. Removed; git history has it if a device-
   * only load fault ever needs it again (git log -S snes_diag.txt). */

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
    /* The return value used to be dropped. A refused state -- and the loader
     * refuses correctly, on magic, version AND payload length -- then starts
     * the ROM cold, silently, and a measurement arm that believed it was
     * resuming a savestate is quietly looking at the title screen instead.
     * That scene is far cheaper than gameplay: it read +2.44 emulated and
     * +10.5 DRAWN fps and was mistaken for an optimisation, three bracketed
     * runs deep. Any struct change anywhere in the state alters the payload
     * length, so this fires more often than it looks like it should.
     * Published as a counter so a bench can refuse to believe an arm that
     * did not resume. */
    g_snes_state_resumed = odroid_system_emu_load_state(save_slot) ? 1u : 0u;
    if (!g_snes_state_resumed)
      printf("snes: savestate refused (slot %d) -- running COLD\n", (int)save_slot);
  } else {
    lcd_clear_buffers();
  }
  memset(snes_frame, 0, sizeof(snes_frame));   /* borders start black and stay black */

#ifdef SNES_DEVICE_PROFILE
  /* Arm AFTER snes_init()/load_state so the AHB pre-flight sees the Apu's
   * 66 KB already taken, and after the last boot-time SD write. */
  snes_profile_init(SNES_AUDIO_RATE, SNES_AUDIO_SAMPLES);
  uint32_t prof_wall_prev = snes_prof_wall_now();
  uint32_t prof_dma_prev  = dma_counter;
#endif
#if SNES_FRAME_HIST
  common_emu_enable_dwt_cycles();   /* free-running; nothing in this loop clears it */
#endif

  while (1) {
#ifdef SNES_DEVICE_PROFILE
    /* ONE DWT base per iteration. Every SNES_PROF_MARK below is a CUMULATIVE
     * read from it and nothing in the loop may re-clear CYCCNT — a nested
     * clear would silently reset the frame's zero point, so
     * snes_profile_record() checks the marks are monotonic and fails the whole
     * run if they are not. */
    uint32_t prof_base       = common_emu_get_dwt_cycles();
    uint32_t prof_apu_in_emu = 0;
    uint32_t prof_wall_pace  = 0;
    uint32_t prof_dma_before = 0;
    uint32_t prof_wfi        = 0;
#endif
    wdog_refresh();

#ifdef GNW_AUTOSAVE_FRAME
    /* Measurement-only: write slot 0 once, N frames in, then carry on.
     *
     * A/B on hardware needs a scene both arms see identically, and the only
     * thing that gives that is a savestate. Two of the three SNES states on the
     * card are four bytes too short for this build (see g_snes_state_refuse) and
     * making a new one by hand means playing the console with a debug probe
     * soldered to it. So the console makes it: boot, run N deterministic frames,
     * save, and every later arm resumes exactly there.
     *
     * The SD write is a one-shot at a known frame, through the same call the
     * menu uses -- not a write inside the play loop, which this project forbids
     * for good reason (FAT corruption). Never enabled in a shipping build. */
    {
      static bool autosaved = false;
      if (!autosaved && snes->frames >= (uint32_t)GNW_AUTOSAVE_FRAME) {
        autosaved = true;
        odroid_audio_mute(true);
        printf("snes: autosave slot 0 at frame %lu -> %d\n",
               (unsigned long)snes->frames, (int)odroid_system_emu_save_state(0));
        odroid_audio_mute(false);
        common_emu_state.startup_frames = 0;
      }
    }
#endif

    bool drawFrame = common_emu_frame_loop();
    /* The benchmark counts EMULATED frames, and the overload guard draws only
     * one in four -- so a change that makes skipped frames cheaper lets the
     * guard draw more, which raises what the player sees while LOWERING the
     * number bench.sh prints. Anything aimed at the skipped-frame path has to be
     * read against this counter, not against fps. */
    if (drawFrame) g_snes_drawn_frames++;
    SNES_PROF_MARK(SNES_PROF_M_FRAMECTL);

    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, &blit);
    common_emu_input_loop_handle_turbo(&joystick);

    snes->input1->currentState = read_snes_pad(&joystick);
    SNES_PROF_MARK(SNES_PROF_M_INPUT);

    g_ppu_skip_render = !drawFrame;
    render_frame_into_active_buffer();   /* arm the line callback every frame */
    SNES_PROF_MARK(SNES_PROF_M_RENDER_ARM);

    run_frame_events(snes);
    SNES_PROF_MARK(SNES_PROF_M_EMU);
#if SNES_FRAME_HIST
    snes_fh_mark = snes_frame_hist_now();
#endif
#ifdef SNES_DEVICE_PROFILE
    /* Split the Ledger B APU accumulator at the emu/pcm boundary: core_rem is
     * emu_outer minus the APU work that happened INSIDE emu_outer, and the pcm
     * top-up further down must not be subtracted from it as well. */
    prof_apu_in_emu = snes_prof_b_apu_cyc;
#endif

    if (drawFrame) {
      present_frame();   /* OFF scaling: kicks an async DMA2D copy, doesn't wait */
    }
    SNES_PROF_MARK(SNES_PROF_M_PRESENT_KICK);

    /* Audio runs while the DMA2D copy above is still in flight in the AXI SRAM
     * background — genuine overlap, not just moving the same blocking wait
     * around. present_frame_wait() below is what actually drains it, right
     * before anything else touches the destination buffer. */
    snes_pcm_submit();
    SNES_PROF_MARK(SNES_PROF_M_PCM);

    if (drawFrame) {
      present_frame_wait();
      SNES_PROF_MARK(SNES_PROF_M_PRESENT_TAIL);
      common_ingame_overlay();
      SNES_PROF_MARK(SNES_PROF_M_OVERLAY);
      lcd_swap();
      SNES_PROF_MARK(SNES_PROF_M_SWAP);
    }
#ifdef SNES_DEVICE_PROFILE
    else {
      /* Skipped frame: none of the three drawn-only phases ran. Collapse their
       * marks onto the pcm mark so each delta is exactly 0 -- leaving them at
       * last frame's values would make the deltas garbage AND break the
       * monotonicity gate. */
      snes_prof_mark[SNES_PROF_M_PRESENT_TAIL] =
      snes_prof_mark[SNES_PROF_M_OVERLAY]      =
      snes_prof_mark[SNES_PROF_M_SWAP]         = snes_prof_mark[SNES_PROF_M_PCM];
    }
    uint32_t prof_pace_w0 = snes_prof_wall_now();
#endif

    /* Pace the loop by the audio DMA UNCONDITIONALLY (WS pattern,
     * main_wswan.c:348-366). common_emu_sound_sync skips this wait when
     * skip_frames>0, which let heavy SNES games run ahead of real time and
     * play audio at fast-forward speed (SMW 55fps = 92% speed, SM 31fps =
     * half speed). Waiting for one DMA tick every frame caps the emulator at
     * real time so the tempo is correct; on frames that genuinely overran,
     * the DMA has already advanced so this passes through with no delay.
     *
     * Catch-up: if the DMA advanced multiple periods during a slow frame,
     * produce extra audio batches so the next periods have fresh data. Stale
     * audio from the underrun period itself is irrecoverable, but this
     * prevents compounding — both half-buffers end up fresh. HLE (SMW)
     * produces audio cheaply (no SPC700); LLE (Zelda/SM) pays ~0.5ms per
     * extra batch (one DSP frame of apu_cycle). Port sync for LLE: extra
     * apu_cycle calls advance SPC700 beyond the CPU frame; SPC700 reads
     * stale $2140-43 ports — inaudible for music (N-SPC polls ports
     * periodically, not per-sample). */
    /* SNES_PACE_OFF=1: diagnostic arm only. The pacing wait below caps the loop
     * at the audio-DMA period, so measured fps is a count of periods, not a
     * measure of how fast the emulator is: a frame that finishes early sleeps
     * out the remainder and a frame that overruns costs two. That makes an
     * optimisation worth 5% of the frame worth almost nothing on the counter,
     * which is exactly what the first device A/B of the merged opcode entry
     * showed. This flag removes the cap so the raw emulation rate is visible
     * and the distance to 60.15 can be read as work rather than as luck.
     * Never ship it: without the wait the emulator free-runs and the audio
     * plays at whatever speed the silicon manages. */
#ifndef SNES_PACE_OFF
#define SNES_PACE_OFF 0
#endif
/* SNES_PACE_RING: SHELVED, NEVER RUN ON HARDWARE IN THIS FORM. The first
 * version of this idea waited on the backlog INSTEAD of the tick, took the
 * device down with a Hardfault (CFSR=0x01000400: imprecise bus fault, so the
 * reported PC was drain-time noise and named nothing), and the repeated
 * benchmark resets that followed pushed the anti-brick counter to its limit.
 * The cause was never identified. What is below is the conservative rewrite --
 * the tick condition stays, the backlog can only cut a wait short -- but it is
 * a rewrite of code whose failure is not understood, which is not the same as a
 * fix. Do not enable it before the BSOD can name a fault: ABFSR at 0xE000EFA8
 * (which bus the wild access used) and SCB->SHCSR fault enables (so the title
 * says Busfault instead of Hardfault). See docs/SNES_LAST_MILE.md. */
#ifndef SNES_PACE_RING
#define SNES_PACE_RING 0
#endif
/* The backlog below which the loop stops waiting for its tick and gets on with
 * the next frame. The stretcher holds the ring near TARGET (640) and starts
 * playing filler when it runs dry, so the line sits one frame (266 samples)
 * under target: far enough down to mean "a slow frame really did eat into the
 * cushion", far enough above empty that catching up still has room to work. */
#ifndef SNES_PACE_RING_LOW
#define SNES_PACE_RING_LOW 374u
#endif
#if SNES_FRAME_HIST
    /* Frame-work histogram. The paced frame counter cannot see this: it counts
     * audio periods, so a frame at 0.9 periods and one at 0.1 read the same and
     * one at 1.1 costs two. What decides the paced rate is therefore not the
     * average frame -- it is how many frames land past the period line, and
     * nothing in the build could say which those were.
     *
     * One DWT read per frame, bucketed into RAM. Nothing is written to the SD
     * card (forbidden during play) and nothing is printed: the arrays are read
     * out over SWD by tools/gnw_probe/frame_hist.py while the game runs. */
    {
      uint32_t now = snes_frame_hist_now();
      if (snes_fh_last) {
        uint32_t work = now - snes_fh_last;
        uint32_t emu  = snes_fh_mark - snes_fh_last;
        uint32_t rest = now - snes_fh_mark;
        uint32_t b = work >> SNES_FH_SHIFT;
        snes_fh_bucket[b < SNES_FH_BUCKETS ? b : SNES_FH_BUCKETS - 1]++;
        snes_fh_sum += work;
        if (work > snes_fh_max) snes_fh_max = work;
        snes_fh_n++;
        /* The period line in cycles, from the clock the loop actually runs at. */
        if (work > (uint32_t)(SystemCoreClock / 6015u) * 100u) {
          snes_fh_emu_over += emu; snes_fh_rest_over += rest; snes_fh_n_over++;
          if (!drawFrame) snes_fh_skipped_over++;
        } else {
          snes_fh_emu_under += emu; snes_fh_rest_under += rest; snes_fh_n_under++;
        }
      }
    }
#endif
    /* The line below is a MARKER: tests/test_snes_audio_pacing.sh extracts this
     * whole block by matching it verbatim and brace-matching to its close. Do
     * not fold anything into that condition -- putting `!SNES_PACE_OFF &&`
     * inside the parentheses made the extractor return nothing and turned the
     * pacing test red, which is exactly the loud failure it is built to give. */
    if (SNES_PACE_OFF) { /* diagnostic arm: no pacing at all */ } else
    if (odroid_system_get_app()->speedupEnabled == SPEEDUP_1x) {
        static uint32_t snes_last_dma = 0;
        if (snes_last_dma == 0) snes_last_dma = dma_counter;
#ifdef SNES_DEVICE_PROFILE
        /* How many audio periods had ALREADY elapsed when we got here. 0 means
         * we arrived before the deadline and are about to wait; >=1 means the
         * deadline had already passed, the wait below falls straight through,
         * and LLE never recovers that period. Reading a near-zero wait as
         * "we overran" without this number is the mistake the review flagged. */
        prof_dma_before = dma_counter - snes_last_dma;
#endif
        /* Feed the watchdog and give up eventually. This wait blocks until the
         * audio DMA advances, and it had neither guard: if dma_counter stops
         * moving the loop spins forever, WWDG fires, and a watchdog reset
         * leaves no BSOD and no log -- the device simply drops out.
         *
         * That is what killed SMW after closing the pause menu. The comment
         * above is the reason it was SMW-only: a frame that overran finds the
         * DMA already advanced and passes straight through, so Zelda (35 fps)
         * and Super Metroid (30 fps) never actually wait here. Only SMW with
         * the audio HLE is fast enough to arrive before the DMA ticks -- and
         * if the menu left the DMA stopped, that arrival never ends. Turning
         * the HLE off "fixed" it by making the emulator too slow to reach the
         * wait, which is why every other explanation fit some of the evidence
         * and none of it fit all.
         *
         * The bound is generous (~100 ms, several audio periods): a frame that
         * legitimately waits is far under it, so pacing is unchanged. */
        {
            uint32_t spin_guard = 0;
#if SNES_PACE_RING
            /* Wait on the audio BACKLOG, not on the tick.
             *
             * Waiting for a tick gives every frame its own period and lets it
             * keep none of what it does not use, so the loop runs at
             * 1/E[max(work, T)] rather than 1/E[work]. Measured on hardware
             * with the per-frame histogram those are 57.4 and 60.7 fps: the
             * median frame is 14.8 ms against a 16.625 ms period, but 23% of
             * frames run past the line and each of those costs its full length
             * while every fast frame is still rounded up to a whole period.
             * Borrowing across the boundary does not happen by itself -- a slow
             * frame leaves the next one only 12.3 ms to the following tick, and
             * a median frame does not fit in that either.
             *
             * The stretcher ring is the buffer that can absorb it: 2048 samples
             * deep, held near TARGET (640 = 2.4 frames) by the ISR pulling one
             * half-buffer per period. Pacing on it says the only true rule --
             * do not produce audio faster than it is consumed -- and says
             * nothing about WHEN inside that budget a frame runs. A slow frame
             * spends backlog; the fast frames after it earn it back.
             *
             * The tick condition STAYS, and is what makes this safe. Waiting on
             * the backlog alone can wait for a drain that is not coming: before
             * the stretcher primes, the ISR pulls nothing, so the ring only
             * fills and the loop sits in the guard for a hundred thousand WFIs
             * per frame. That is not a hypothesis -- it is what the first
             * backlog-only build did, and the device never reached the
             * benchmark's first frame. So the rule is: wait for the tick as
             * before, but stop waiting early when the backlog says we are
             * behind. It can only ever remove waiting, never add it. */
            while (dma_counter == snes_last_dma
                   && snes_stretch_fill() > SNES_PACE_RING_LOW
                   && spin_guard < 100000u) {
                wdog_refresh();
                cpumon_sleep();
                spin_guard++;
            }
#else
            while (dma_counter == snes_last_dma && spin_guard < 100000u) {
                wdog_refresh();
                cpumon_sleep();
                spin_guard++;
            }
#endif
#ifdef SNES_DEVICE_PROFILE
            prof_wfi = spin_guard;   /* __WFI() round trips actually executed */
#endif
        }
        uint32_t elapsed = dma_counter - snes_last_dma;
        /* Advancing the reference by one period instead of to `dma_counter`
         * looked like free frame rate -- keep what a fast frame did not use --
         * and measured 57.10 against 57.40 on hardware, three runs each. The
         * reason it cannot help is arithmetic: this counter has one tick per
         * 16.625 ms, and the slow frames are 21-24 ms, so they advance it by
         * exactly ONE tick just like a fast frame. The two rules only differ
         * for a frame past 33 ms, which the histogram says does not happen.
         * Slack is real but it is sub-tick, and no integer counter can hold it.
         * That is why SNES_PACE_RING waits on the audio backlog instead: the
         * backlog is measured in samples, and a sample is 1/16th of a
         * millisecond. */
        snes_last_dma = dma_counter;
        /* LLE catch-up. A frame slower than one 16.625 ms audio period leaves
         * the periods it ran past playing stale buffer contents -- the comment
         * below used to call that an accepted underrun, and the device profile
         * showed it on every single frame (deadline advance 1 on 34, 2 on 30,
         * 0 on none). Fill those periods too. This does NOT call apu_cycle, so
         * the SPC700 timer never moves relative to the 65816 and the tempo
         * objection below does not apply: the samples come from the stretcher,
         * which is resampling audio the core already produced.
         * Backlog past a few periods is a pause/load, not a slow frame -- drop
         * it and resync rather than grind, same rule (and the same watchdog-fed,
         * bounded wait) the wire path settled on after the SMW-menu death. */
        /* ...but do NOT wait for each of those periods to arrive. Waiting here
         * quantises the frame to whole 16.625 ms audio periods: a frame whose
         * work takes 1.67 periods sleeps out the remaining third and costs two,
         * so a game that could run at 36 fps is pinned near 30. Mario Kart
         * measured 28.3 fps at 78.7% CPU busy on the device -- a fifth of the
         * time asleep while missing every deadline, which is that stall. The
         * periods this loop is "catching up" have already been played; nothing
         * can be put back into them. All the emit does is drain the stretcher
         * ring at the rate the DMA actually consumed it, which keeps the
         * stretcher's rate estimate honest, and that costs microseconds.
         *
         * Set SNES_PACE_CATCHUP_WAIT=1 to get the stall back. The wait is still
         * watchdog-fed and bounded there: it was unbounded once and killed SMW
         * after the pause menu (a reset with no BSOD and no log). */
#ifndef SNES_PACE_CATCHUP_WAIT
#define SNES_PACE_CATCHUP_WAIT 0
#endif
        /* And do NOT emit for them either. snes_pcm_emit() fills ONE audio-DMA
         * buffer, and there is exactly one of those per period: calling it
         * twice inside the same period writes the same buffer twice -- only
         * the last is ever heard -- while pulling a full buffer out of the
         * stretcher ring each time. That drains the ring two or three times
         * faster than the DMA consumes it, so it is permanently dry and what
         * plays is filler rather than the game. It is why the sound came back
         * "완전히 다른 소리" and not merely broken.
         *
         * Dropping the wait without dropping the emit was my error, not the
         * original design's: the wait was what kept one emit inside one period.
         * With neither, a frame that spans several periods simply leaves the
         * periods it missed playing the previous buffer, and the deficit shows
         * up where it belongs -- in the stretcher's measured rate. */
        (void)elapsed;
        /* Catch-up only for HLE: wire_frame_audio produces samples without
         * advancing the SPC700, so extra batches are free and tempo stays
         * exact. For LLE, extra apu_cycle calls would drift the SPC700's
         * internal timer relative to the CPU, changing music tempo (the
         * port-sync-drift the user flagged). LLE accepts the underrun
         * (stale audio for one DMA period on slow frames) rather than
         * distorting tempo — the WS unconditional wait above already
         * guarantees correct playback speed. */
#ifdef SNES_SMW_HLE_PRODUCT
        if (g_wire_on) {
            /* Catch-up: replay the audio batches the DMA advanced past on a slow
             * frame. But `elapsed` is unbounded -- after the pause menu it counts
             * every audio period spent in the menu, so this tried to regenerate
             * seconds of audio at once, and the inner DMA wait below had no
             * watchdog feed or ceiling. That is the real SMW-menu death (Codex
             * adversarial review of 15dd53c8, which had guarded the wrong wait).
             *
             * Backlog past a couple of frames is not worth replaying -- the audio
             * for time spent paused is gone regardless -- so cap it and resync
             * rather than grind through it. */
            if (elapsed > 4) {
                snes_last_dma = dma_counter;   /* drop the backlog, don't replay it */
                elapsed = 0;
            }
            while (elapsed > 1) {
                snes_pcm_submit();
                uint32_t guard = 0;
                while (dma_counter == snes_last_dma && guard < 20000u) {
                    wdog_refresh();
                    cpumon_sleep();
                    guard++;
                }
                snes_last_dma = dma_counter;
                elapsed--;
            }
        }
#endif
    }
#if SNES_FRAME_HIST
    /* Restart the clock AFTER the wait, so a bucket is emulation work and never
     * the sleep that follows it. */
    snes_fh_last = snes_frame_hist_now();
#endif
#ifdef SNES_DEVICE_PROFILE
    prof_wall_pace = snes_prof_wall_now() - prof_pace_w0;
    SNES_PROF_MARK(SNES_PROF_M_PACING);
    {
      /* wall_frame is measured end-of-iteration to end-of-iteration, so it
       * covers the WHOLE period including the previous snes_profile_record()
       * call. Measuring it from the top of this iteration instead would leave
       * the recorder's own cost outside every frame, and the sum would then
       * disagree with the audio-DMA reference for a reason that has nothing to
       * do with the emulator -- i.e. it would break the wall_vs_dma gate that
       * exists to catch real problems. */
      uint32_t wall_now = snes_prof_wall_now();
      uint32_t dma_now  = dma_counter;
      snes_profile_record(drawFrame, prof_base, prof_apu_in_emu,
                          wall_now - prof_wall_prev, prof_wall_pace,
                          prof_dma_before, dma_now - prof_dma_prev, prof_wfi);
      prof_wall_prev = wall_now;
      prof_dma_prev  = dma_now;
    }
#endif
  }
}

#ifdef RIG_CALL_PROFILE
/* snes.c's read/write classification hooks reference these as extern. The rigs
 * define them; the firmware never did, so on hardware the one number we had --
 * snes_cpuRead is 10.5% of a scrolling frame -- had no breakdown behind it.
 * Diagnostic builds only (SNES_READ_PROFILE=1); read them over SWD. */
uint64_t g_cpuRead_calls, g_win_cpuRead_calls, g_cpuRead_slow, g_cpuRead_romhit, g_cpuRead_wram;
uint64_t g_cpuWrite_calls, g_cpuWrite_slow;
uint64_t g_dma_cycle_calls, g_win_dma_cycle_calls, g_dma_cycle_true, g_dma_doDma_calls, g_dma_doHdma_calls;
#endif
