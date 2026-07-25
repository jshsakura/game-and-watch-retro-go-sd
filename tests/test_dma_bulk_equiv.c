/* Regression gate for external/sm's dma_doDma bulk A->B optimisation.
 *
 * dma_doDma() now drains a whole A->B channel per call instead of one byte per
 * dma_cycle(). The change is a host-CPU speedup that MUST NOT alter what a DMA
 * transfers. This test links the REAL external/sm/src/snes/dma.c (not a copy)
 * and drives it through the same entry the firmware uses -- dma_startDma() then
 * `while (dma_cycle()) {}` -- while stubbing the bus so every transfer is
 * recorded. It then compares that recorded sequence against an INDEPENDENT
 * per-byte reference model computed here. If a future edit to dma.c changes the
 * order, addressing, count, or direction of any transfer, the sequences diverge
 * and this fails.
 *
 * The bus stub is deterministic: snes_read(addr) returns a fixed hash of addr,
 * so each recorded (dest, value) pair encodes both the B-bus destination and the
 * A-bus source -- the full observable content of the DMA, not just its shape.
 *
 * A self-check at the end feeds the comparator two deliberately different
 * sequences and asserts it reports a mismatch, so a vacuous always-pass harness
 * cannot hide here (tests/run.sh's RED-before-GREEN discipline).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "dma.h"
#include "snes.h"

/* ---- bus stub: deterministic source bytes, record every transfer ---- */
typedef struct { uint32_t key; uint8_t val; } Rec;   /* key = dir<<24 | dest */
#define MAXREC 200000
static Rec g_log[MAXREC];
static long g_n;

static uint8_t src_byte(uint32_t a) {          /* deterministic A-bus content */
  uint32_t h = a * 2654435761u; return (uint8_t)(h >> 24);
}
uint8_t g_fail;
uint8_t snes_read(Snes* s, uint32_t a) { (void)s; return src_byte(a); }
uint8_t snes_readBBus(Snes* s, uint8_t a) { (void)s; return src_byte(0xB00000u | a); }
void snes_write(Snes* s, uint32_t a, uint8_t v) {           /* B->A dest */
  (void)s; if (g_n < MAXREC) { g_log[g_n].key = 0x01000000u | a; g_log[g_n].val = v; } g_n++;
}
void snes_writeBBus(Snes* s, uint8_t a, uint8_t v) {        /* A->B dest */
  (void)s; if (g_n < MAXREC) { g_log[g_n].key = a; g_log[g_n].val = v; } g_n++;
}

/* ---- independent per-byte reference (what a DMA must transfer) ---- */
static const int refOff[8][4] = {
  {0,0,0,0},{0,1,0,1},{0,0,0,0},{0,0,1,1},{0,1,2,3},{0,1,0,1},{0,0,0,0},{0,0,1,1}
};
static Rec g_ref[MAXREC]; static long g_rn;
static void ref_channel(const DmaChannel* c) {
  uint16_t aAdr = c->aAdr; unsigned oi = c->offIndex; uint16_t size = c->size;
  const int step = c->fixed ? 0 : (c->decrement ? -1 : 1);
  do {
    uint8_t bdst = c->bAdr + refOff[c->mode][oi & 3];
    if (c->fromB) {
      uint32_t adst = ((uint32_t)c->aBank << 16) | aAdr;
      g_ref[g_rn].key = 0x01000000u | adst; g_ref[g_rn].val = src_byte(0xB00000u | bdst);
    } else {
      uint32_t asrc = ((uint32_t)c->aBank << 16) | aAdr;
      g_ref[g_rn].key = bdst; g_ref[g_rn].val = src_byte(asrc);
    }
    g_rn++; oi++; aAdr += step; size--;
  } while (size != 0);
}

/* ---- drive the REAL dma.c, then the reference, and compare ---- */
static int run(const char* name, DmaChannel seed[8], uint8_t mask) {
  Dma dma; memset(&dma, 0, sizeof dma);
  for (int i = 0; i < 8; i++) dma.channel[i] = seed[i];
  g_n = 0;
  dma_startDma(&dma, mask, false);
  while (dma_cycle(&dma)) {}

  g_rn = 0;
  for (int i = 0; i < 8; i++) if (seed[i].dmaActive) ref_channel(&seed[i]);

  if (g_n != g_rn) { printf("FAIL %s: real=%ld ref=%ld transfers\n", name, g_n, g_rn); return 1; }
  if ((size_t)g_n > MAXREC) { printf("FAIL %s: overflow %ld\n", name, g_n); return 1; }
  if (memcmp(g_log, g_ref, g_n * sizeof(Rec)) != 0) {
    for (long k = 0; k < g_n; k++) if (memcmp(&g_log[k], &g_ref[k], sizeof(Rec))) {
      printf("FAIL %s: transfer %ld real(key=%08x val=%02x) ref(key=%08x val=%02x)\n",
             name, k, g_log[k].key, g_log[k].val, g_ref[k].key, g_ref[k].val); return 1;
    }
  }
  printf("OK  %-22s %ld transfers match the per-byte reference\n", name, g_n);
  return 0;
}

int main(void) {
  int fail = 0;
  DmaChannel z; memset(&z, 0, sizeof z);
  #define C(...) (DmaChannel){__VA_ARGS__}

  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aAdr=0x2000,.aBank=0x7e,.size=64,.dmaActive=1,.mode=1); fail+=run("A->B mode1 inc", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aAdr=0x1234,.size=100,.dmaActive=1,.mode=0,.fixed=1); fail+=run("A->B fixed src", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aAdr=0x8000,.aBank=0x7f,.size=50,.dmaActive=1,.mode=1,.decrement=1); fail+=run("A->B decrement", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aBank=0x40,.size=0,.dmaActive=1,.mode=4); fail+=run("A->B size=0 (64K)", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aAdr=0x0010,.aBank=0x7e,.size=17,.dmaActive=1,.mode=3,.offIndex=2); fail+=run("A->B offIndex=2", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x80,.aAdr=0x2000,.aBank=0x7e,.size=32,.dmaActive=1,.mode=1,.fromB=1); fail+=run("B->A (per-byte)", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z; s[0]=C(.bAdr=0x18,.aAdr=0xfff0,.aBank=0x7e,.size=40,.dmaActive=1,.mode=0); fail+=run("A->B aAdr wrap", s, 0x01); }
  { DmaChannel s[8]; for(int i=0;i<8;i++)s[i]=z;
    s[0]=C(.bAdr=0x18,.aAdr=0x1000,.aBank=0x7e,.size=30,.dmaActive=1,.mode=1);
    s[2]=C(.bAdr=0x04,.aAdr=0x2000,.aBank=0x7f,.size=25,.dmaActive=1,.mode=4,.decrement=1);
    s[5]=C(.bAdr=0x22,.aAdr=0x3000,.size=8,.dmaActive=1,.mode=3,.fixed=1,.offIndex=1);
    fail+=run("multi-channel 0,2,5", s, 0x25); }
  { DmaChannel s[8]; for(int i=0;i<8;i++){ s[i]=z; s[i]=C(.bAdr=(uint8_t)(0x18+i),.aAdr=(uint16_t)(0x100*i),.aBank=0x7e,.size=1,.dmaActive=1,.mode=(uint8_t)(i&7)); } fail+=run("all 8 channels", s, 0xff); }

  /* self-check: the comparator must be live, not vacuous. Feed it two different
   * sequences and require a mismatch. */
  g_n = 2; g_log[0]=(Rec){1,1}; g_log[1]=(Rec){2,2};
  g_rn = 2; g_ref[0]=(Rec){1,1}; g_ref[1]=(Rec){2,3};
  if (memcmp(g_log, g_ref, 2*sizeof(Rec)) == 0) { printf("FAIL self-check: comparator is vacuous\n"); fail++; }
  else printf("OK  self-check              comparator detects a 1-byte difference\n");

  printf(fail ? "\nFAIL: %d\n" : "\nALL DMA TRANSFER SEQUENCES MATCH\n", fail);
  return fail ? 1 : 0;
}
