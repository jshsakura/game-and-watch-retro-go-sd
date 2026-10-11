/* Experimental generic SH2 arithmetic span. No game, PC or ROM identity.
 * Included after mame/sh2.c so the reference opcode primitives are shared.
 * Only one-cycle instructions with no memory, control-flow, SR-mask change
 * or callback are allowed. Fresh opcode reads avoid a code invalidation cache.
 */
static inline __attribute__((always_inline)) int generic_alu_op(SH2 *s, unsigned op)
{
  unsigned n=(op>>8)&15, m=(op>>4)&15;
  switch (op>>12) {
  case 7: ADDI(s,op&255,n); return 1;
  case 14: MOVI(s,op&255,n); return 1;
  case 2:
    switch(op&15) {
    case 8: TST(s,m,n); break;
    case 9: AND(s,m,n); break;
    case 10: XOR(s,m,n); break;
    case 11: OR(s,m,n); break;
    case 12: CMPSTR(s,m,n); break;
    case 13: XTRCT(s,m,n); break;
    case 14: MULU(s,m,n); break;
    case 15: MULS(s,m,n); break;
    default: return 0;
    } return 1;
  case 3:
    switch(op&15) {
    case 0: CMPEQ(s,m,n); break;
    case 2: CMPHS(s,m,n); break;
    case 3: CMPGE(s,m,n); break;
    case 6: CMPHI(s,m,n); break;
    case 7: CMPGT(s,m,n); break;
    case 8: SUB(s,m,n); break;
    case 10: SUBC(s,m,n); break;
    case 11: SUBV(s,m,n); break;
    case 12: ADD(s,m,n); break;
    case 14: ADDC(s,m,n); break;
    case 15: ADDV(s,m,n); break;
    default: return 0;
    } return 1;
  case 6:
    switch(op&15) {
    case 3: MOV(s,m,n); break;
    case 7: NOT(s,m,n); break;
    case 8: SWAPB(s,m,n); break;
    case 9: SWAPW(s,m,n); break;
    case 10: NEGC(s,m,n); break;
    case 11: NEG(s,m,n); break;
    case 12: EXTUB(s,m,n); break;
    case 13: EXTUW(s,m,n); break;
    case 14: EXTSB(s,m,n); break;
    case 15: EXTSW(s,m,n); break;
    default: return 0;
    } return 1;
  case 4:
    /* Match the reference's low-six-bit decoder, including its aliases. */
    switch(op&63) {
    case 0x00: SHLL(s,n); break;
    case 0x01: SHLR(s,n); break;
    case 0x04: ROTL(s,n); break;
    case 0x05: ROTR(s,n); break;
    case 0x08: SHLL2(s,n); break;
    case 0x09: SHLR2(s,n); break;
    case 0x10: DT(s,n); break;
    case 0x11: CMPPZ(s,n); break;
    case 0x15: CMPPL(s,n); break;
    case 0x18: SHLL8(s,n); break;
    case 0x19: SHLR8(s,n); break;
    case 0x20: SHAL(s,n); break;
    /* SHAR's existing run-fold reads future opcodes and changes pc/icount;
     * it must stay in the original interpreter rather than this pure span. */
    case 0x24: ROTCL(s,n); break;
    case 0x25: ROTCR(s,n); break;
    case 0x28: SHLL16(s,n); break;
    case 0x29: SHLR16(s,n); break;
    default: return 0;
    } return 1;
  case 8:
    if((op&0xff00)!=0x8800) return 0;
    CMPIM(s,op&255); return 1;
  case 12:
    switch((op>>8)&15) {
    case 8: TSTI(s,op&255); break;
    case 9: ANDI(s,op&255); break;
    case 10: XORI(s,op&255); break;
    case 11: ORI(s,op&255); break;
    default: return 0;
    } return 1;
  default: return 0;
  }
}

static unsigned __attribute__((noinline)) generic_alu_span(SH2 *s, unsigned op)
{
  unsigned pc=s->ppc, mask, region=pc&0xdf000000u;
  const unsigned char *base;
  if(s->delay || s->test_irq || s->icount<=0 || (pc&1)) return 0;
  if(region==gnw_fw_rom.region) {
    /* The map uses a byte mask (usually size-1). An even instruction PC
     * gives the same fetch with bit 0 cleared; normalize to a word mask. */
    mask=gnw_fw_rom.mask & ~1u; base=gnw_fw_rom.base;
    if(!base || mask>0x00fffffeu || (mask&(mask+2))) return 0;
  } else if(region==0x06000000u) {
    mask=0x3fffe; base=s->p_sdram;
    if(!base) return 0;
  } else return 0;
  unsigned avail=(mask+2-(pc&mask))>>1;
  unsigned budget=(unsigned)s->icount;
  if(avail>budget) avail=budget;
  const unsigned short *p=(const unsigned short *)(base+(pc&mask));
  unsigned done=0;
  do {
    if(!generic_alu_op(s,op)) break;
    done++;
    if(done==avail) break;
    op=p[done];
  } while(1);
  if(done) {
    s->ppc=pc+2*(done-1); s->pc=pc+2*done; s->icount-=done;
#ifdef RIG_SH2_COUNT
    /* The outer interpreter already counted the first opcode. */
    g_sh2_insns+=done-1;
#endif
  }
  return done;
}
