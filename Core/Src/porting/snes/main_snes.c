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

#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/ppu.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/input.h"
#include "snes/saveload.h"

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

static void cpu_tick(Snes *s) {
  if (dma_cycle(s->dma)) return;
  if (s->cpuCyclesLeft == 0) {
    s->cpuMemOps = 0;
    int cycles = cpu_runOpcode(s->cpu);
    s->cpuCyclesLeft += (cycles - s->cpuMemOps) * 6;
  }
  s->cpuCyclesLeft -= 2;
}

static void run_dots(Snes *s, int dots) {
  while (dots > 0) {
    if (s->dma->dmaBusy || s->dma->hdmaTimer > 0) {
      dma_cycle(s->dma);
      s->apuCatchupCycles += apuCyclesPerMaster * 2.0;
      s->hPos += 2; dots -= 2; continue;
    }
    bool started_dma = false;
    if (s->cpuCyclesLeft == 0) {
      apply_irq_match(s);
      s->cpuMemOps = 0;
      int cycles = cpu_runOpcode(s->cpu);
      s->cpuCyclesLeft += (cycles - s->cpuMemOps) * 6;
      started_dma = s->dma->dmaBusy || s->dma->hdmaTimer > 0;
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
 * 32 px left margin and NO top margin — exactly the harness placement. An
 * overscan title renders up to 239 lines; a top margin would run the last
 * rows past the framebuffer. */
static void render_frame_into_active_buffer(void) {
  uint16_t *fb = lcd_get_active_buffer();
  PpuBeginDrawing(snes->ppu, (uint8_t *)(fb + 32), 320 * 2, 0);
}

static void blit(void) {
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

  bool ok = (rom != NULL) && !cart_needs_coprocessor(rom, sz) &&
            snes_loadRom(snes, rom, (int)sz);
  if (!ok) {
    /* No lang_t entry on purpose: strings are positional in the SD language
     * binaries and this core is experimental — English only for now. */
    odroid_overlay_alert("Unsupported SNES cartridge (coprocessor/mapper)");
    odroid_system_switch_app(0);   /* back to launcher */
    return;
  }
  /* The loader mallocs a second copy of the ROM; the flash-cached image is
   * already memory-mapped and read-only for the core — point cart->rom back
   * at it and free the copy (same trick as the M7 rig; saves up to 6 MB). */
  if (snes->cart->rom && snes->cart->rom != snes_rom) {
    free(snes->cart->rom);
    snes->cart->rom = (uint8_t *)snes_rom;
  }

  if (load_state) {
    odroid_system_emu_load_state(save_slot);
  } else {
    lcd_clear_buffers();
  }

  while (1) {
    wdog_refresh();

    bool drawFrame = common_emu_frame_loop();

    odroid_input_read_gamepad(&joystick);
    common_emu_input_loop(&joystick, options, &blit);
    common_emu_input_loop_handle_turbo(&joystick);

    snes->input1->currentState = read_snes_pad(&joystick);

    g_ppu_skip_render = !drawFrame;
    if (drawFrame)
      render_frame_into_active_buffer();
    run_frame_events(snes);

    if (drawFrame) {
      blit();
      lcd_swap();
    }

    snes_pcm_submit();

    common_emu_sound_sync(false);
  }
}
