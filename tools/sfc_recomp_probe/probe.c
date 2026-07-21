/* 65816 static-recompiler feasibility probe — the recording half.
 *
 * cpu_runOpcode (sed-instrumented copy of external/sm/src/snes/cpu.c) calls
 * probe_op(cpu, opcode) right before cpu_doOpcode and probe_post(cpu) right
 * after. From those two hooks we reconstruct everything a translator needs to
 * know about a real ROM's executed code:
 *
 *  - executed-site map with M/X flag states seen (flag polymorphism)
 *  - ROM vs WRAM execution split (WRAM code = copied/self-modifying = fallback)
 *  - exact code-byte extents: a sequentially-followed instruction's length is
 *    the PC delta (exact, flag-aware); control transfers use a fixed table
 *    (all 65816 transfer opcodes have flag-independent lengths)
 *  - basic-block leaders (any PC reached by a non-sequential step)
 *  - indirect-jump sites (0x6C/0x7C/0xDC/0xFC) with distinct-target sets
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "src/snes/cpu.h"

#define SEEN_EXEC 1u
#define SEEN_M0   2u
#define SEEN_M1   4u
#define SEEN_X0   8u
#define SEEN_X1   16u
#define SEEN_LEAD 32u

static uint8_t *g_seen;    /* 16M, one per 24-bit address (opcode sites) */
static uint8_t *g_bytes;   /* 16M, bit0 = byte belongs to an executed instruction */

static uint64_t g_ops_total, g_ops_rom, g_ops_wram, g_ops_other;
static uint64_t g_leaders;
static uint64_t g_exec_6c, g_exec_7c, g_exec_dc, g_exec_fc, g_exec_40;
static uint64_t g_seq_unmatched; /* transfers whose length we couldn't mark (interrupts) */

static uint32_t g_expected = 0xffffffff;   /* next sequential opcode address */
static uint32_t g_last_pc24 = 0xffffffff;  /* previous instruction address */
static uint8_t  g_last_op;
static uint32_t g_cur_pc24;
static uint8_t  g_cur_op;

/* ---- indirect-jump site tracking ------------------------------------- */
#define MAX_SITES 8192
#define MAX_TGTS  64
typedef struct {
  uint32_t site;
  uint8_t  op;
  uint8_t  overflow;
  uint16_t ntgt;
  uint64_t execs;
  uint32_t tgts[MAX_TGTS];
} ISite;
static ISite g_sites[MAX_SITES];
static int g_nsites;

static ISite *site_get(uint32_t site, uint8_t op) {
  for (int i = 0; i < g_nsites; i++)
    if (g_sites[i].site == site) return &g_sites[i];
  if (g_nsites >= MAX_SITES) return NULL;
  ISite *s = &g_sites[g_nsites++];
  s->site = site; s->op = op;
  return s;
}
static void site_target(uint32_t site, uint8_t op, uint32_t tgt) {
  ISite *s = site_get(site, op);
  if (!s) return;
  for (int i = 0; i < s->ntgt; i++)
    if (s->tgts[i] == tgt) return;
  if (s->ntgt < MAX_TGTS) s->tgts[s->ntgt++] = tgt;
  else s->overflow = 1;
}

/* All 65816 control-transfer opcodes have flag-independent lengths. 0 = not a
 * transfer (an interrupt redirected the flow); skip, the site gets its bytes
 * marked on any uninterrupted execution. */
static int transfer_len(uint8_t op) {
  switch (op) {
  case 0x10: case 0x30: case 0x50: case 0x70:
  case 0x80: case 0x90: case 0xB0: case 0xD0: case 0xF0: return 2; /* branches */
  case 0x82: return 3;                                   /* BRL */
  case 0x4C: case 0x6C: case 0x7C: case 0xDC: case 0xFC:
  case 0x20: return 3;                                   /* JMP/JSR forms */
  case 0x5C: case 0x22: return 4;                        /* JML/JSL long */
  case 0x60: case 0x6B: case 0x40: return 1;             /* RTS/RTL/RTI */
  case 0x00: case 0x02: return 2;                        /* BRK/COP */
  case 0xCB: case 0xDB: return 1;                        /* WAI/STP */
  case 0x44: case 0x54: return 3;                        /* MVP/MVN repeat in place:
                                                            landing on themselves is
                                                            non-sequential, not an
                                                            interrupt */
  default: return 0;
  }
}

/* WRAM if bank $7E/$7F or the low-RAM mirror in banks $00-$3F/$80-$BF. */
static int is_wram(uint32_t pc24) {
  uint8_t bank = pc24 >> 16;
  uint16_t a16 = pc24 & 0xffff;
  if (bank == 0x7e || bank == 0x7f) return 1;
  return ((bank & 0x7f) < 0x40) && a16 < 0x2000;
}
/* ROM for the LoROM games we probe: upper half of any non-WRAM bank. */
static int is_rom(uint32_t pc24) {
  uint8_t bank = pc24 >> 16;
  uint16_t a16 = pc24 & 0xffff;
  if (bank == 0x7e || bank == 0x7f) return 0;
  return a16 >= 0x8000;
}

void probe_op(Cpu *cpu, uint8_t opcode) {
  if (!g_seen) {
    g_seen = calloc(1u << 24, 1);
    g_bytes = calloc(1u << 24, 1);
    if (!g_seen || !g_bytes) { fprintf(stderr, "probe: out of memory\n"); exit(1); }
  }
  uint32_t pc24 = (((uint32_t)cpu->k << 16) | (uint16_t)(cpu->pc - 1)) & 0xffffff;
  g_cur_pc24 = pc24; g_cur_op = opcode;

  g_ops_total++;
  if (is_wram(pc24)) g_ops_wram++;
  else if (is_rom(pc24)) g_ops_rom++;
  else g_ops_other++;

  uint8_t *e = &g_seen[pc24];
  *e |= SEEN_EXEC;
  *e |= cpu->mf ? SEEN_M1 : SEEN_M0;
  *e |= cpu->xf ? SEEN_X1 : SEEN_X0;

  switch (opcode) {
  case 0x6C: g_exec_6c++; break;
  case 0x7C: g_exec_7c++; break;
  case 0xDC: g_exec_dc++; break;
  case 0xFC: g_exec_fc++; break;
  case 0x40: g_exec_40++; break;
  }

  /* Control-transfer detection. A post-execution "expected PC" cannot work: by
   * the time we see it, the jump has landed and the next opcode IS at that PC —
   * everything looks sequential. Instead judge by the PREVIOUS opcode: every
   * 65816 control-transfer opcode has a flag-independent length, so its
   * fall-through address is g_last_pc24 + transfer_len. Landing anywhere else
   * means the transfer was taken and pc24 is a block leader. A non-transfer
   * opcode can only be followed non-sequentially by an interrupt vector. */
  if (g_last_pc24 != 0xffffffff) {
    int tlen = transfer_len(g_last_op);
    if (tlen) {
      /* transfer opcode: mark its bytes; taken iff we didn't fall through */
      for (int i = 0; i < tlen; i++) g_bytes[(g_last_pc24 + i) & 0xffffff] |= 1;
      if (pc24 != ((g_last_pc24 + tlen) & 0xffffff)) {
        if (!(*e & SEEN_LEAD)) { *e |= SEEN_LEAD; g_leaders++; }
        if (g_last_op == 0x6C || g_last_op == 0x7C || g_last_op == 0xFC || g_last_op == 0xDC) {
          site_target(g_last_pc24, g_last_op, pc24);
          ISite *s = site_get(g_last_pc24, g_last_op);
          if (s) s->execs++;
        }
      }
    } else {
      uint32_t len = (pc24 - g_last_pc24) & 0xffffff;
      if (len >= 1 && len <= 4) {
        /* sequential: exact instruction length from the PC delta (flag-aware) */
        for (uint32_t i = 0; i < len; i++) g_bytes[(g_last_pc24 + i) & 0xffffff] |= 1;
      } else {
        /* interrupt hijacked the flow mid-stream; vector target is a leader */
        g_seq_unmatched++;
        if (!(*e & SEEN_LEAD)) { *e |= SEEN_LEAD; g_leaders++; }
      }
    }
  }
}

void probe_post(Cpu *cpu) {
  g_expected = (((uint32_t)cpu->k << 16) | cpu->pc) & 0xffffff;
  g_last_pc24 = g_cur_pc24;
  g_last_op = g_cur_op;
}

void probe_report(void) {
  if (!g_seen) { printf("probe: nothing executed\n"); return; }
  uint64_t sites_rom = 0, sites_wram = 0, sites_other = 0;
  uint64_t bytes_rom = 0, bytes_wram = 0, bytes_other = 0;
  uint64_t mpoly_rom = 0, xpoly_rom = 0, mpoly_wram = 0, xpoly_wram = 0;
  for (uint32_t a = 0; a < (1u << 24); a++) {
    uint8_t e = g_seen[a];
    if (e & SEEN_EXEC) {
      int w = is_wram(a), r = is_rom(a);
      if (w) sites_wram++; else if (r) sites_rom++; else sites_other++;
      int mp = (e & SEEN_M0) && (e & SEEN_M1);
      int xp = (e & SEEN_X0) && (e & SEEN_X1);
      if (w) { mpoly_wram += mp; xpoly_wram += xp; }
      else if (r) { mpoly_rom += mp; xpoly_rom += xp; }
    }
    if (g_bytes[a] & 1) {
      if (is_wram(a)) bytes_wram++; else if (is_rom(a)) bytes_rom++; else bytes_other++;
    }
  }
  printf("[probe] opcodes total=%llu rom=%llu (%.2f%%) wram=%llu (%.2f%%) other=%llu (%.4f%%)\n",
         (unsigned long long)g_ops_total,
         (unsigned long long)g_ops_rom, 100.0 * g_ops_rom / g_ops_total,
         (unsigned long long)g_ops_wram, 100.0 * g_ops_wram / g_ops_total,
         (unsigned long long)g_ops_other, 100.0 * g_ops_other / g_ops_total);
  printf("[probe] unique sites rom=%llu wram=%llu other=%llu | code bytes rom=%llu wram=%llu other=%llu\n",
         (unsigned long long)sites_rom, (unsigned long long)sites_wram, (unsigned long long)sites_other,
         (unsigned long long)bytes_rom, (unsigned long long)bytes_wram, (unsigned long long)bytes_other);
  printf("[probe] flag-poly ROM sites: M=%llu X=%llu | WRAM: M=%llu X=%llu\n",
         (unsigned long long)mpoly_rom, (unsigned long long)xpoly_rom,
         (unsigned long long)mpoly_wram, (unsigned long long)xpoly_wram);
  printf("[probe] block leaders=%llu | transfer-len misses (interrupts)=%llu\n",
         (unsigned long long)g_leaders, (unsigned long long)g_seq_unmatched);
  printf("[probe] indirect execs: 6C=%llu 7C=%llu DC=%llu FC=%llu RTI=%llu\n",
         (unsigned long long)g_exec_6c, (unsigned long long)g_exec_7c,
         (unsigned long long)g_exec_dc, (unsigned long long)g_exec_fc,
         (unsigned long long)g_exec_40);
  int max_tgt = 0, n6c = 0, n7c = 0, ndc = 0, nfc = 0, noverflow = 0;
  for (int i = 0; i < g_nsites; i++) {
    ISite *s = &g_sites[i];
    if (s->ntgt > max_tgt) max_tgt = s->ntgt;
    noverflow += s->overflow;
    switch (s->op) {
    case 0x6C: n6c++; break;
    case 0x7C: n7c++; break;
    case 0xDC: ndc++; break;
    case 0xFC: nfc++; break;
    }
  }
  printf("[probe] indirect sites: 6C=%d 7C=%d DC=%d FC=%d | max distinct targets=%d overflow(>%d)=%d\n",
         n6c, n7c, ndc, nfc, max_tgt, MAX_TGTS, noverflow);
  /* per-site detail for the fat ones */
  for (int i = 0; i < g_nsites; i++) {
    ISite *s = &g_sites[i];
    if (s->ntgt >= 8 || s->overflow)
      printf("[probe]   site %02x:%04x op=%02x targets=%d%s execs=%llu\n",
             s->site >> 16, s->site & 0xffff, s->op, s->ntgt,
             s->overflow ? "+" : "", (unsigned long long)s->execs);
  }
}
