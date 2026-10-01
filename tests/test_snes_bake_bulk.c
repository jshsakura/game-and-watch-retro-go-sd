/* Differential span gate against the pre-experiment source, not a rewritten
 * model. Covers partial opcodes, event boundaries and DMA changing the flag. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "spin_bake.h"
#include "dma.h"

extern SpinBake ref_g_bake;
extern int ref_spin_bake_run_span(Snes *, Cpu *, int);

typedef struct {
  Snes s;
  Cpu c;
  Dma d;
  uint8_t ram[0x2000];
  unsigned dma_calls;
  unsigned dma_effect;
} Fixture;

static Fixture a, b;
static unsigned cases;
static uint32_t rng = 0x51a26001u;

static uint32_t next_random(void) {
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

/* A deterministic DMA dependency. It can write the polled byte or raise an
 * interrupt when a burst ends, so folding across DMA would fail this gate. */
bool dma_cycle(Dma *d) {
  Fixture *f = d == &a.d ? &a : &b;
  f->dma_calls++;
  if (d->hdmaTimer) d->hdmaTimer -= 2;
  if (d->dmaBusy && d->dmaTimer) {
    d->dmaTimer--;
    d->dmaBusy = d->dmaTimer != 0;
  }
  if (!d->dmaBusy && !d->hdmaTimer) {
    if (f->dma_effect == 1) f->ram[0x12] = 0x80;
    if (f->dma_effect == 2) f->c.nmiWanted = true;
    if (f->dma_effect == 3) f->c.irqWanted = true;
  }
  return true;
}

static void bind(Fixture *f) {
  f->s.cpu = &f->c; f->s.dma = &f->d; f->s.ram = f->ram;
  f->c.mem = &f->s; f->d.snes = &f->s;
}

static void init_case(void) {
  memset(&a, 0, sizeof(a));
  memset(&g_bake, 0, sizeof(g_bake));
  bind(&a);
  a.c.pc = 0x8034; a.c.a = 0xa5ff; a.c.mf = true;
  a.c.z = false; a.c.n = true;
  a.s.hPos = 2; a.s.vPos = 120;
  g_bake.on = true; g_bake.armed = true;
  g_bake.pc_load = 0x8034; g_bake.pc_branch = 0x8036;
  g_bake.bank = 0; g_bake.bank_alt = 0x80;
  g_bake.dp_off = 0x12; g_bake.mf = 1;
  g_bake.charge_load = 24; g_bake.charge_branch = 22;
}

static void compare_case(int dots) {
  b = a; bind(&b);
  ref_g_bake = g_bake;
  const int expected = ref_spin_bake_run_span(&a.s, &a.c, dots);
  const int actual = spin_bake_run_span(&b.s, &b.c, dots);
  /* Pointers intentionally identify two independent machines. */
  Snes sa = a.s, sb = b.s;
  Cpu ca = a.c, cb = b.c;
  Dma da = a.d, db = b.d;
  sa.cpu = sb.cpu = NULL; sa.dma = sb.dma = NULL;
  sa.ram = sb.ram = NULL; ca.mem = cb.mem = NULL;
  da.snes = db.snes = NULL;
  if (expected != actual || memcmp(&sa, &sb, sizeof(sa)) ||
      memcmp(&ca, &cb, sizeof(ca)) || memcmp(&da, &db, sizeof(da)) ||
      memcmp(a.ram, b.ram, sizeof(a.ram)) ||
      memcmp(&ref_g_bake, &g_bake, sizeof(g_bake)) ||
      a.dma_calls != b.dma_calls) {
    fprintf(stderr, "FAIL case=%u dots=%d returned=%d/%d pc=%04x/%04x "
            "cycles=%u/%u h=%u/%u apu=%u/%u laps=%u/%u dma=%u/%u\n",
            cases, dots, expected, actual, a.c.pc, b.c.pc,
            a.s.cpuCyclesLeft, b.s.cpuCyclesLeft, a.s.hPos, b.s.hPos,
            a.s.apuDotsAccum, b.s.apuDotsAccum, ref_g_bake.laps,
            g_bake.laps, a.dma_calls, b.dma_calls);
    assert(0);
  }
  cases++;
}

int main(void) {
  /* Every budget through a complete scanline, both instruction PCs, and
   * every partial charge from 0 through the LDA's 24 cycles. */
  for (unsigned pc = 0; pc < 2; pc++)
    for (unsigned carry = 0; carry <= 24; carry++)
      for (int dots = -2; dots <= 1364; dots++) {
        init_case();
        a.c.pc += pc * 2; a.c.z = pc != 0;
        a.s.cpuCyclesLeft = carry;
        compare_case(dots);
      }

  /* A new event between spans can write the flag or raise an IRQ. It must
   * be observed on re-entry, including when the last opcode is incomplete. */
  for (unsigned event = 0; event < 3; event++)
    for (unsigned budget = 2; budget <= 512; budget += 2) {
      init_case(); compare_case(budget);
      /* compare_case advances a with the reference and b with the candidate.
       * Their equality lets a become the next span's initial machine. */
      if (event == 0) a.ram[0x12] = 1;
      if (event == 1) a.c.nmiWanted = true;
      if (event == 2) { a.s.vIrqEnabled = true; a.s.vTimer = a.s.vPos; }
      g_bake = ref_g_bake;
      compare_case(512);
    }

  for (unsigned i = 0; i < 40000; i++) {
    init_case();
    const unsigned kind = next_random() % 20;
    a.c.pc += (next_random() % 3) * 2;
    a.c.z = next_random() & 1; a.c.a = next_random();
    a.s.cpuCyclesLeft = next_random() & 0xff;
    a.s.hPos = next_random(); a.s.apuDotsAccum = next_random();
    g_bake.laps = next_random();
    if (kind == 0) a.c.nmiWanted = true;
    if (kind == 1) a.c.irqWanted = true;
    if (kind == 2) a.c.waiting = true;
    if (kind == 3) a.c.stopped = true;
    if (kind == 4) a.s.hIrqEnabled = true;
    if (kind == 5) { a.s.vIrqEnabled = true; a.s.vTimer = a.s.vPos; }
    if (kind == 6) a.c.k = 0x80; /* mirror is valid */
    if (kind == 7) a.c.k = 0x7e; /* WRAM bank is not the mirror */
    if (kind == 8) a.c.mf = false;
    if (kind == 9) a.c.dp = 1;
    if (kind == 10) a.c.dp = 0x2000;
    if (kind == 11) { a.c.dp = 0x1f00; g_bake.dp_off = 0xff; }
    if (kind == 12) { a.c.dp = 0xff00; g_bake.dp_off = 0xff; }
    if (kind == 13) a.ram[0x12] = 0x80;
    if (kind == 14) { a.d.dmaBusy = true; a.d.dmaTimer = 1 + next_random() % 12; }
    if (kind == 15) a.d.hdmaTimer = 2 * (1 + next_random() % 12);
    if (kind == 16) { a.d.hdmaTimer = 2; a.dma_effect = 1; }
    if (kind == 17) { a.d.hdmaTimer = 4; a.dma_effect = 2; }
    if (kind == 18) { a.d.dmaBusy = true; a.d.dmaTimer = 2; a.dma_effect = 3; }
    if (kind == 19) {
      g_bake.charge_load = 2 * (1 + next_random() % 127);
      g_bake.charge_branch = 2 * (1 + next_random() % 127);
    }
    compare_case(next_random() % 1365);
  }
  printf("PASS: %u differential spans against original source\n", cases);
  return 0;
}
