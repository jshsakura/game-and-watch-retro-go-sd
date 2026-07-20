/* N-SPC dialect tables + hooks for the generalized spc_player engine.
 *
 * The engine (generated copy of SM's spc_player.c) speaks exactly the standard
 * 0xE0-based vcmd set. A variant stream is adapted at read time:
 *   - nspc_xlat_note():  variant note encoding -> standard (tie/rest/percussion)
 *   - nspc_remap_vcmd(): variant vcmd opcode  -> standard opcode for
 *     HandleEffect, or consumed here (skips, Konami loop/ADSR) returning 0
 *   - nspc_instr_pitch_base(): instrument tuning per layout (std 2-byte,
 *     earlier 1-byte signed, Konami per-SRCN tuning tables)
 *
 * Dialect data transcribed from VGMTrans (zlib) NinSnesProfile.cpp — see
 * ref/ for the fetched source of truth.
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "spc_player.h"          /* SpcPlayer, Channel */
#include "snes/dsp_regs.h"
#include "nspc_config.h"

struct NspcCfg g_nspc_cfg;

/* ---- standard (0xE0): identity — what the engine natively speaks ---------- */
/* operand counts = spc_player's kEffectByteLength, extended to a full byte map */
static unsigned char std_vlen[256];
static unsigned char std_remap[256];

/* SM's note tables (standard dialect) — duplicated from spc_player.c because
 * they are function-local statics there. */
static const unsigned char kNVolStd[16] = { 25, 50, 76, 101, 114, 127, 140, 152, 165, 178, 191, 203, 216, 229, 242, 252 };
static const unsigned char kGateStd[8]  = { 50, 101, 127, 152, 178, 203, 229, 252 };

static const unsigned char kStdLen[31] = {  /* 0xe0..0xfe, = kEffectByteLength */
  1, 1, 2, 3, 0, 1, 2, 1, 2, 1, 1, 3, 0, 1, 2, 3, 1, 3, 3, 0, 1, 3, 0, 3, 3, 3, 1, 2, 0, 0, 0};

/* ---- earlier (SMW-era, 0xDA): REORDERED map (VGMTrans applyEarlierSeqDialect) */
static unsigned char smw_vlen[256];
static unsigned char smw_remap[256];
/* VGMTrans kVolumeTableEarlier / kDurTableEarlier */
static const unsigned char kNVolSMW[16] = { 0x08, 0x12, 0x1b, 0x24, 0x2c, 0x35, 0x3e, 0x47, 0x51, 0x5a, 0x62, 0x6b, 0x7d, 0x8f, 0xa1, 0xb3 };
static const unsigned char kGateSMW[8]  = { 0x33, 0x66, 0x80, 0x99, 0xb3, 0xcc, 0xe6, 0xff };

/* ---- konami (GD3): standard base + overrides ------------------------------- */
static unsigned char gd3_vlen[256];
static unsigned char gd3_remap[256];
#define GD3_LOOP_START 0xe5     /* raw opcodes handled natively below */
#define GD3_LOOP_END   0xe6
#define GD3_ADSR_GAIN  0xfb
static uint16_t gd3_loop_start[8];
static uint8_t  gd3_loop_count[8];

/* log dropped opcodes once each so we know what a variant is missing */
static uint32_t skipped_ops[8];   /* bitmask per 32-op block */
static void log_skip(unsigned char op) {
  if (skipped_ops[op >> 5] & (1u << (op & 31))) return;
  skipped_ops[op >> 5] |= 1u << (op & 31);
  fprintf(stderr, "[nspc] skipped vcmd 0x%02x (no standard equivalent)\n", op);
}

unsigned char nspc_xlat_note(unsigned char cmd) {
  if (g_nspc_cfg.vcmdStart != 0xda) return cmd;   /* std/gd3: already standard */
  /* earlier: tie c6->c8, rest c7->c9, percussion d0-d9 -> ca..d3 */
  if (cmd == 0xc6) return 0xc8;
  if (cmd == 0xc7) return 0xc9;
  if (cmd >= 0xd0 && cmd <= 0xd9) return (unsigned char)(0xca + (cmd - 0xd0));
  if (cmd >= 0xc8 && cmd <= 0xcf) { log_skip(cmd); return 0xc9; } /* undefined -> rest */
  return cmd;
}

unsigned char nspc_remap_vcmd(struct SpcPlayer *p, struct Channel *c, unsigned char cmd) {
  /* GD3 opcodes the standard engine has no case for — handled here natively */
  if (g_nspc_cfg.remap == gd3_remap) {
    if (cmd == GD3_LOOP_START) {
      gd3_loop_start[c->index] = c->pattern_order_ptr_for_chan;
      gd3_loop_count[c->index] = 0;
      return 0;
    }
    if (cmd == GD3_LOOP_END) {
      unsigned char times = p->ram[c->pattern_order_ptr_for_chan++];
      c->pattern_order_ptr_for_chan += 2;          /* volume/pitch delta: unused */
      if (++gd3_loop_count[c->index] != times && gd3_loop_start[c->index])
        c->pattern_order_ptr_for_chan = gd3_loop_start[c->index];
      return 0;
    }
    if (cmd == GD3_ADSR_GAIN) {
      unsigned char adsr1 = p->ram[c->pattern_order_ptr_for_chan++];
      unsigned char adsr2 = p->ram[c->pattern_order_ptr_for_chan++];
      unsigned char gain  = p->ram[c->pattern_order_ptr_for_chan++];
      if (!(p->is_chan_on & p->cur_chan_bit)) {
        dsp_write(p->dsp, V0ADSR1 + c->index * 16, adsr1);
        dsp_write(p->dsp, V0ADSR2 + c->index * 16, adsr2);
        dsp_write(p->dsp, V0GAIN  + c->index * 16, gain);
      }
      return 0;
    }
  }
  unsigned char std = g_nspc_cfg.remap[cmd];
  if (std) return std;
  /* skip: consume the raw operands so the stream stays aligned */
  log_skip(cmd);
  c->pattern_order_ptr_for_chan += g_nspc_cfg.vlen[cmd];
  return 0;
}

unsigned short nspc_instr_pitch_base(struct SpcPlayer *p, const unsigned char *ip) {
  if (g_nspc_cfg.stride == 5)                    /* earlier: signed high byte only */
    return (unsigned short)(ip[4] << 8);
  if (g_nspc_cfg.tunCnt > 0) {                   /* GD3: per-SRCN tuning tables */
    unsigned char srcn = ip[0];
    signed char coarse = 0; unsigned char fine = 0;
    if (!(srcn & 0x80) && srcn < g_nspc_cfg.tunCnt) {
      coarse = (signed char)p->ram[(g_nspc_cfg.tunLow + srcn) & 0xffff];
      fine   = p->ram[(g_nspc_cfg.tunLow + g_nspc_cfg.tunCnt + srcn) & 0xffff];
    }
    /* engine multiplier (256 = x1): 256 * (4045/4286) * 2^((coarse+fine/256)/12)
     * — inverse of VGMTrans's Konami tuning->cents conversion */
    double m = 256.0 * (4045.0 / 4286.0) * exp2(((double)coarse + fine / 256.0) / 12.0);
    if (m < 1) m = 1;
    if (m > 65535) m = 65535;
    return (unsigned short)(m + 0.5);
  }
  return (unsigned short)(ip[4] << 8 | ip[5]);   /* standard 6-byte entry */
}

/* ---- variant selection ------------------------------------------------------ */
static void fill_common(unsigned char *remap, unsigned char *vlen) {
  for (int i = 0; i < 256; i++) { remap[i] = 0; vlen[i] = 0; }
}

void nspc_variant_std(void) {
  fill_common(std_remap, std_vlen);
  /* VGMTrans's cross-game std map only documents through 0xfa, but SM's own
   * HandleEffect() (spc_player.c) implements real cases through 0xfe --
   * 0xfb (skip-2 marker), 0xfc (cutk), 0xfd/0xfe (fast_forward, which
   * Music_HandleCmdFromSnes's `do {...} while (p->fast_forward)` loop
   * depends on). Treating them as "no standard equivalent" silently dropped
   * fast_forward triggers a song needs. kEffectByteLength[27..30] in
   * spc_player.c (the operand-byte counts) already agreed with kStdLen's
   * tail for these ops -- only the remap-to-"do nothing" was wrong, not the
   * stream alignment, so this is a safe, provably-correct extension, not a
   * guess: HandleEffect's own switch statement is the ground truth for what
   * these opcodes do. On real Super Metroid this measurably fixed a gross
   * symptom (rapid, nonsensical incrementing song IDs after swap -- the
   * downstream effect of the stream desync this caused) but did NOT fully
   * fix the deeper issue: the swapped-in player still eventually drifts
   * into a state where on-screen content stops updating, well past where
   * pure LLE has already recovered. See nspc_wire.c's wire_swap() comment
   * and the memory writeup for the open investigation. */
  for (int op = 0xe0; op <= 0xfe; op++) {
    std_remap[op] = (unsigned char)op;
    std_vlen[op] = kStdLen[op - 0xe0];
  }
  std_vlen[0xff] = 0;  /* HandleEffect has no 0xff case (falls to Not_Implemented(),
                         * and kStdLen has no [31] entry) -- must stay skip+log. */
  g_nspc_cfg.vcmdStart = 0xe0; g_nspc_cfg.tieOp = 0xc8; g_nspc_cfg.callOp = 0xef;
  g_nspc_cfg.pslideOp = 0xf9; g_nspc_cfg.stride = 6; g_nspc_cfg.baseAddr = 0;
  g_nspc_cfg.tunLow = 0; g_nspc_cfg.tunCnt = 0;
  g_nspc_cfg.remap = std_remap; g_nspc_cfg.vlen = std_vlen;
  g_nspc_cfg.nvol = kNVolStd; g_nspc_cfg.gate = kGateStd;
}

void nspc_variant_earlier(void) {
  fill_common(smw_remap, smw_vlen);
  /* VGMTrans applyEarlierSeqDialect: raw op -> standard equivalent */
  static const unsigned char map[][2] = {
    {0xda, 0xe0}, {0xdb, 0xe1}, {0xdc, 0xe2}, {0xdd, 0xf9}, {0xde, 0xe3},
    {0xdf, 0xe4}, {0xe0, 0xe5}, {0xe1, 0xe6}, {0xe2, 0xe7}, {0xe3, 0xe8},
    {0xe4, 0xe9}, {0xe5, 0xeb}, {0xe6, 0xec}, {0xe7, 0xed}, {0xe8, 0xee},
    {0xe9, 0xef}, {0xea, 0xf0}, {0xeb, 0xf1}, {0xec, 0xf2}, {0xee, 0xf4},
    {0xef, 0xf5}, {0xf0, 0xf6}, {0xf1, 0xf7}, {0xf2, 0xf8},
  };
  for (unsigned i = 0; i < sizeof(map) / 2; i++) {
    smw_remap[map[i][0]] = map[i][1];
    smw_vlen[map[i][0]] = kStdLen[map[i][1] - 0xe0];   /* same event, same operands */
  }
  /* 0xed, 0xf3..0xff: unmapped in the earlier dialect -> skip 0 bytes, log */
  g_nspc_cfg.vcmdStart = 0xda; g_nspc_cfg.tieOp = 0xc6; g_nspc_cfg.callOp = 0xe9;
  g_nspc_cfg.pslideOp = 0xdd; g_nspc_cfg.stride = 5; g_nspc_cfg.baseAddr = 0;
  g_nspc_cfg.tunLow = 0; g_nspc_cfg.tunCnt = 0;
  g_nspc_cfg.remap = smw_remap; g_nspc_cfg.vlen = smw_vlen;
  g_nspc_cfg.nvol = kNVolSMW; g_nspc_cfg.gate = kGateSMW;
}

void nspc_variant_gd3(int baseAddr, int tunLow, int tunCnt) {
  fill_common(gd3_remap, gd3_vlen);
  for (int op = 0xe0; op <= 0xfa; op++) {
    gd3_remap[op] = (unsigned char)op;
    gd3_vlen[op] = kStdLen[op - 0xe0];
  }
  /* VGMTrans applyDerivedSeqOverrides(Konami) */
  gd3_remap[0xe4] = 0; gd3_vlen[0xe4] = 2;   /* unknown, 2 operands */
  gd3_remap[0xe5] = 0; gd3_vlen[0xe5] = 0;   /* loop start  (handled natively) */
  gd3_remap[0xe6] = 0; gd3_vlen[0xe6] = 3;   /* loop end    (handled natively) */
  gd3_remap[0xe8] = 0; gd3_vlen[0xe8] = 0;   /* nop */
  gd3_remap[0xe9] = 0; gd3_vlen[0xe9] = 0;   /* nop */
  gd3_remap[0xf5] = 0; gd3_vlen[0xf5] = 0;   /* unknown0 x4 (echo cmds differ) */
  gd3_remap[0xf6] = 0; gd3_vlen[0xf6] = 0;
  gd3_remap[0xf7] = 0; gd3_vlen[0xf7] = 0;
  gd3_remap[0xf8] = 0; gd3_vlen[0xf8] = 0;
  gd3_remap[0xfb] = 0; gd3_vlen[0xfb] = 3;   /* ADSR+GAIN   (handled natively) */
  gd3_remap[0xfc] = 0; gd3_vlen[0xfc] = 0;   /* nop x3 */
  gd3_remap[0xfd] = 0; gd3_vlen[0xfd] = 0;
  gd3_remap[0xfe] = 0; gd3_vlen[0xfe] = 0;
  g_nspc_cfg.vcmdStart = 0xe0; g_nspc_cfg.tieOp = 0xc8; g_nspc_cfg.callOp = 0xef;
  g_nspc_cfg.pslideOp = 0xf9; g_nspc_cfg.stride = 6; g_nspc_cfg.baseAddr = baseAddr;
  g_nspc_cfg.tunLow = tunLow; g_nspc_cfg.tunCnt = tunCnt;
  g_nspc_cfg.remap = gd3_remap; g_nspc_cfg.vlen = gd3_vlen;
  g_nspc_cfg.nvol = kNVolStd; g_nspc_cfg.gate = kGateStd;
  for (int i = 0; i < 8; i++) { gd3_loop_start[i] = 0; gd3_loop_count[i] = 0; }
}
