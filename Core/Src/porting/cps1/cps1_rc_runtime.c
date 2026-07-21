#include "cps1_rc_runtime.h"

static uint32_t size_mask(unsigned size)
{
    switch (size) {
    case 0:  return 0xFFu;
    case 1:  return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

void cps1_rc_add(cps1_cpu68k_t *regs, unsigned reg, uint32_t data, unsigned size)
{
    uint32_t mask = size_mask(size);
    uint32_t result = cps1_cpu68k_add_flags(regs->d[reg], data, size, &regs->sr);
    regs->d[reg] = (regs->d[reg] & ~mask) | (result & mask);
}

void cps1_rc_sub(cps1_cpu68k_t *regs, unsigned reg, uint32_t data, unsigned size)
{
    uint32_t mask = size_mask(size);
    uint32_t result = cps1_cpu68k_sub_flags(regs->d[reg], data, size, &regs->sr);
    regs->d[reg] = (regs->d[reg] & ~mask) | (result & mask);
}

void cps1_rc_set_nz_flags(cps1_cpu68k_t *regs, int32_t value)
{
    regs->sr = (uint16_t)(regs->sr & ~(CPS1_CPU68K_SR_N | CPS1_CPU68K_SR_Z |
                                        CPS1_CPU68K_SR_V | CPS1_CPU68K_SR_C));
    if (value == 0) regs->sr |= CPS1_CPU68K_SR_Z;
    if (value < 0)  regs->sr |= CPS1_CPU68K_SR_N;
}

int cps1_rc_cc_T(const cps1_cpu68k_t *regs)  { return cps1_cpu68k_cc_test(regs->sr, 0); }
int cps1_rc_cc_F(const cps1_cpu68k_t *regs)  { return cps1_cpu68k_cc_test(regs->sr, 1); }
int cps1_rc_cc_HI(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 2); }
int cps1_rc_cc_LS(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 3); }
int cps1_rc_cc_CC(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 4); }
int cps1_rc_cc_CS(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 5); }
int cps1_rc_cc_NE(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 6); }
int cps1_rc_cc_EQ(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 7); }
int cps1_rc_cc_VC(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 8); }
int cps1_rc_cc_VS(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 9); }
int cps1_rc_cc_PL(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 10); }
int cps1_rc_cc_MI(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 11); }
int cps1_rc_cc_GE(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 12); }
int cps1_rc_cc_LT(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 13); }
int cps1_rc_cc_GT(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 14); }
int cps1_rc_cc_LE(const cps1_cpu68k_t *regs) { return cps1_cpu68k_cc_test(regs->sr, 15); }

void cps1_rc_fallback_step(cps1_cpu68k_t *regs, uint32_t addr)
{
    regs->pc = addr;
    cps1_cpu68k_step(regs);
}
