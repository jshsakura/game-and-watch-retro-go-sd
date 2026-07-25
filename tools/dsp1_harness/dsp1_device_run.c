/* DSP-1 HLE device-shaped reproduction harness.
 *
 * Drives dsp1_hle.c's PUBLIC byte-level protocol (dsp1_writeDR/readDR) --
 * the same interface cart.c uses -- across every command index the chip's
 * dispatch table recognizes (0x00-0x3f; mirrors fold per-group in
 * cmd_canon(), everything else incl. $80 is a protocol NOP), with several
 * representative/edge parameter sets per command, plus a dedicated
 * multi-line Raster streaming run (state 3: dsp1_readDR auto-advances the
 * scanline and recomputes on every 4-word boundary, matching what a real
 * Mode-7 frame does once per scanline) and a pinned regression case for
 * the cmd_inverse underflow hang.
 *
 * Built with the DEVICE's compile-time reality (-DTARGET_GNW) and
 * -fsanitize=address,alignment via dsp1_run.sh -- see that script for why:
 * this project has already caught one real device fault this way (a 64-bit
 * store through a misaligned pointer that only ARM's LDRD/STRD traps on;
 * see CLAUDE.md's "Testing a core the way the device runs it" and
 * tools/sm_harness/device_run.sh, same pattern, applied here).
 *
 * What this harness can and cannot catch, learned on its first outing:
 *   - It CANNOT see overlay-layout bugs (the atan/.overlay_nes_fceu
 *     busfault): those exist only in the device link, and a host binary
 *     has no overlay layout at all. The linker script is the only witness.
 *   - It DID catch the cmd_inverse infinite loop (cmd 0x10, INT16_MIN
 *     exponent): the sweep stalled at the same command twice with zero
 *     sanitizer output -- a hang signature, which is why dsp1_run.sh
 *     treats its own timeout as a first-class FAIL, not an infra hiccup.
 *
 * Output contract: progress goes to STDOUT only. STDERR is reserved for
 * sanitizer reports, so the runner can gate on "stderr contains sanitizer
 * signatures" without fighting the harness's own chatter. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "dsp1_hle.h"

/* dsp1_alloc()/dsp1_size() are NOT declared in dsp1_hle.h -- in the real
 * firmware they only ever get a visible prototype from cart.c's own
 * __attribute__((weak)) stub definitions (which double as declarations for
 * cart.c's later call sites in the SAME translation unit). This harness
 * doesn't compile cart.c, so without explicit prototypes here the calls
 * would be implicit int -- truncating calloc()'s 64-bit host pointer to 32
 * bits, exactly the ahb_malloc()/SpcPlayer_Create bug class CLAUDE.md
 * documents (reproduced here on day one: calloc returned 0x50c000000040,
 * the truncated 0x40 SEGV'd dsp1_reset's memset). */
extern Dsp1* dsp1_alloc(void);
extern uint32_t dsp1_size(void);

/* cmd_canon shape mirror -- dsp1_hle.c's cmd_canon() is static, so the
 * harness drives the real byte protocol and needs its own copy of the same
 * shape table to know how many parameter words to feed each command before
 * it fires. Kept in exact sync with dsp1_hle.c's cmd_canon(); if that ever
 * changes and this doesn't, the harness starts feeding wrong byte counts
 * and desyncs (the state machine treats a stray byte as a new command) --
 * itself a useful canary. Commands not in the table (incl. 0x80) are NOPs:
 * the chip consumes the byte and stays idle. */
static void io_shape_mirror(uint8_t cmd, uint8_t *inW, uint8_t *outW) {
  switch (cmd) {
    case 0x00:                                  *inW = 2; *outW = 1; return;
    case 0x20:                                  *inW = 2; *outW = 1; return;
    case 0x10: case 0x30:                       *inW = 2; *outW = 2; return;
    case 0x04: case 0x24:                       *inW = 2; *outW = 2; return;
    case 0x08:                                  *inW = 3; *outW = 2; return;
    case 0x18: case 0x38:                       *inW = 4; *outW = 1; return;
    case 0x28:                                  *inW = 3; *outW = 1; return;
    case 0x0c: case 0x2c:                       *inW = 3; *outW = 2; return;
    case 0x1c: case 0x3c:                       *inW = 6; *outW = 3; return;
    case 0x02: case 0x12: case 0x22: case 0x32: *inW = 7; *outW = 4; return;
    case 0x0a: case 0x1a: case 0x2a: case 0x3a: *inW = 1; *outW = 4; return;
    case 0x06: case 0x16: case 0x26: case 0x36: *inW = 3; *outW = 3; return;
    case 0x0e: case 0x1e: case 0x2e: case 0x3e: *inW = 2; *outW = 2; return;
    case 0x01: case 0x05: case 0x31: case 0x35:
    case 0x11: case 0x15:
    case 0x21: case 0x25:                       *inW = 4; *outW = 0; return;
    case 0x0d: case 0x09: case 0x39: case 0x3d:
    case 0x1d: case 0x19:
    case 0x2d: case 0x29:                       *inW = 3; *outW = 3; return;
    case 0x03: case 0x33: case 0x13: case 0x23: *inW = 3; *outW = 3; return;
    case 0x0b: case 0x3b: case 0x1b: case 0x2b: *inW = 3; *outW = 1; return;
    case 0x14: case 0x34:                       *inW = 6; *outW = 3; return;
    case 0x0f: case 0x07:                       *inW = 1; *outW = 1; return;
    case 0x2f: case 0x27:                       *inW = 1; *outW = 1; return;
    case 0x1f: case 0x17: case 0x37: case 0x3f: *inW = 1; *outW = 1; return;
    default:                                    *inW = 0; *outW = 0; return;
  }
}

static void drive_command(Dsp1 *d, uint8_t cmd, const uint16_t *params, int nparams) {
  uint8_t inW, outW;
  io_shape_mirror(cmd, &inW, &outW);
  dsp1_writeDR(d, cmd);
  int words_to_send = inW;
  if (nparams < words_to_send) words_to_send = nparams;
  for (int w = 0; w < words_to_send; w++) {
    dsp1_writeDR(d, (uint8_t)(params[w] & 0xff));
    dsp1_writeDR(d, (uint8_t)(params[w] >> 8));
  }
  /* if the caller supplied fewer params than inW wants, pad with zero words
   * so execute() actually fires (the state machine only fires at inWords*2) */
  for (int w = words_to_send; w < inW; w++) {
    dsp1_writeDR(d, 0x00);
    dsp1_writeDR(d, 0x00);
  }
  for (int r = 0; r < outW; r++) {
    (void)dsp1_readDR(d);
    (void)dsp1_readDR(d);
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);   /* progress must survive a hang/kill */
  int failures = 0;

  Dsp1 *d = dsp1_alloc();
  if (!d) { printf("FAIL: dsp1_alloc returned NULL\n"); return 1; }
  printf("[dsp1-harness] boot, sizeof(Dsp1)=%u\n", (unsigned)dsp1_size());

  /* representative / edge parameter sets: zero, all-max-positive,
   * all-max-negative, and a "plausible Mario Kart" mixed set (small
   * coordinates, moderate angles/distances). Fed as raw uint16_t words;
   * the chip interprets them as int16_t per-command. Set 2 (all 0x8000 =
   * INT16_MIN) is the one that exposed the cmd_inverse hang. */
  uint16_t sets[4][8] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff },
    { 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000 },
    { 0x0064, 0x4000, 0x0032, 0x0200, 0x0100, 0x2000, 0x0100, 0x0080 },
  };

  /* --- pinned regression: cmd_inverse underflow hang (fixed in sm
   * gnw-port 1f2203a). a=0x8000 e=0x8000: v underflows to -0.0, 1/v is
   * -Inf, and the pre-fix mantissa loop never terminated. Must return
   * instantly with the saturated a==0-convention outputs. Run FIRST so a
   * regression hangs the harness immediately at a labelled point. --- */
  {
    dsp1_reset(d);
    dsp1_writeDR(d, 0x10);
    dsp1_writeDR(d, 0x00); dsp1_writeDR(d, 0x80);   /* a = 0x8000, LSB first */
    dsp1_writeDR(d, 0x00); dsp1_writeDR(d, 0x80);   /* e = 0x8000 */
    uint16_t o0 = (uint16_t)(dsp1_readDR(d) | (dsp1_readDR(d) << 8));
    uint16_t o1 = (uint16_t)(dsp1_readDR(d) | (dsp1_readDR(d) << 8));
    if (o0 == 0x7fff && o1 == 0x7fff) {
      printf("[dsp1-harness] inverse(0x8000,0x8000) regression: OK (saturated, no hang)\n");
    } else {
      printf("FAIL: inverse(0x8000,0x8000) -> %04x %04x (want 7fff 7fff)\n", o0, o1);
      failures++;
    }
  }

  for (int cmd = 0; cmd < 0x40; cmd++) {
    printf("[dsp1-harness] sweep cmd=0x%02x\n", cmd);
    for (int s = 0; s < 4; s++) {
      dsp1_reset(d);
      /* run a Parameter call first with the same set, so Project/Target/
       * Raster (which read back fx/fy/fz/lfe/les/aas/azs) see a primed,
       * non-default camera state -- matches how a real game always issues
       * Parameter once per frame before any Project/Target/Raster calls. */
      drive_command(d, 0x02, sets[s], 8);
      drive_command(d, (uint8_t)cmd, sets[s], 8);
      (void)dsp1_readSR(d);
    }
  }
  printf("[dsp1-harness] full command x edge-set sweep done (0x00-0x3f x 4 sets, Parameter-primed)\n");

  /* --- pinned regression: the Mario Kart race-init $80 flush. The game
   * writes $80 to DR 128 times before its race-start Parameter -- on the
   * chip that is a protocol NOP that leaves the FSM idle from ANY phase.
   * Pre-fix, $80 parsed as Multiply (the old low-six-bits dispatch), the
   * phantom command ate the real Parameter's command byte, every Raster
   * then ran on reset-default projection state, and the game's per-line
   * tables filled with 32767 -- the flat single-colour road. Reproduce the
   * exact boot shape: leave the FSM mid-command, flush, send the real
   * race-init Parameter, stream a raster line, and require it unsaturated. */
  {
    dsp1_reset(d);
    /* leave the FSM mid-command: a bare Parameter command byte, no params.
     * The residue matters -- with 14 param bytes outstanding, 128 flush
     * bytes land MISALIGNED on the old 5-byte phantom-Multiply cycle
     * (114 % 5 = 4), which is the phase the device actually died in; a
     * residue of 0 mod 5 happens to re-align and would hide the bug. */
    dsp1_writeDR(d, 0x02);
    for (int i = 0; i < 128; i++) dsp1_writeDR(d, 0x80);
    static const uint16_t smk[7] = { 2176, 10144, 0, 64, 256, 0, 0x3400 };
    dsp1_writeDR(d, 0x02);
    for (int w = 0; w < 7; w++) {
      dsp1_writeDR(d, (uint8_t)(smk[w] & 0xff));
      dsp1_writeDR(d, (uint8_t)(smk[w] >> 8));
    }
    (void)dsp1_readDR(d); (void)dsp1_readDR(d);        /* Vof */
    int16_t vva = (int16_t)(dsp1_readDR(d) | (dsp1_readDR(d) << 8));
    (void)dsp1_readDR(d); (void)dsp1_readDR(d);        /* Cx */
    (void)dsp1_readDR(d); (void)dsp1_readDR(d);        /* Cy */
    dsp1_writeDR(d, 0x0a);                             /* Raster from the horizon line */
    dsp1_writeDR(d, (uint8_t)((vva + 1) & 0xff));
    dsp1_writeDR(d, (uint8_t)(((vva + 1) >> 8) & 0xff));
    int16_t A = (int16_t)(dsp1_readDR(d) | (dsp1_readDR(d) << 8));
    for (int b = 2; b < 8; b++) (void)dsp1_readDR(d);
    if (vva < -32 && vva > -128 && A != 32767 && A != -32768 && A != 0) {
      printf("[dsp1-harness] $80-flush + race-init Parameter regression: OK (Vva=%d A=%d)\n", vva, A);
    } else {
      printf("FAIL: $80 flush desynced the FSM (Vva=%d A=%d; pre-fix: Parameter eaten, A=32767)\n", vva, A);
      failures++;
    }
  }

  /* Raster streaming: Parameter once, then Raster command, then keep
   * READING past the first 4-word line -- dsp1_readDR's state==3 branch
   * auto-advances rasterVs and recomputes raster_line() every 8 bytes,
   * exactly what a real Mode-7 frame does once per scanline (up to 224
   * lines). This is the path most likely to accumulate a bad angle/lfe
   * combination across many recomputes in a single command cycle -- and
   * the path the device busfaulted on (raster_line -> ground_dist ->
   * the overlay-captured atan, pre-fb008cf). */
  for (int s = 0; s < 4; s++) {
    dsp1_reset(d);
    drive_command(d, 0x02, sets[s], 8);   /* Parameter: prime the camera */
    dsp1_writeDR(d, 0x0a);                /* Raster command byte */
    dsp1_writeDR(d, 0x00);                /* vs = 0, LSB first */
    dsp1_writeDR(d, 0x00);
    for (int line = 0; line < 240; line++) {
      for (int b = 0; b < 8; b++) (void)dsp1_readDR(d);
    }
    /* terminate the stream the way the chip does: writes drain the pending
     * line's 8 bytes, then the FSM is idle (the reset below covers the rest) */
    dsp1_writeDR(d, 0xff);
  }
  printf("[dsp1-harness] Raster streaming (240 lines x 4 edge sets) done\n");

  /* Project/Target/Raster immediately after reset, no intervening
   * Parameter -- exercises the cold-start state dsp1_reset() leaves
   * (lfe=0x0100, fz=0x0100, rest zeroed), the state a real session is in
   * before its first Parameter. */
  for (int s = 0; s < 4; s++) {
    dsp1_reset(d);
    drive_command(d, 0x06, sets[s], 3);   /* Project, cold */
    dsp1_reset(d);
    drive_command(d, 0x0e, sets[s], 2);   /* Target, cold */
    dsp1_reset(d);
    drive_command(d, 0x0a, sets[s], 1);   /* Raster, cold */
  }
  printf("[dsp1-harness] cold Project/Target/Raster (no prior Parameter) x edge sets done\n");

  free(d);
  if (failures) { printf("[dsp1-harness] FAILED: %d functional check(s)\n", failures); return 1; }
  printf("[dsp1-harness] ALL PASSED, no fault\n");
  return 0;
}
