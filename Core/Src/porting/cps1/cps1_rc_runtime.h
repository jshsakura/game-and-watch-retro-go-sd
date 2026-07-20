#pragma once
/*
 * Runtime support for C emitted by tools/cps1_recompiler/translate.py.
 * Every cps1_rc_* symbol here is called by name from generated code -- see
 * that script's emit_c(). Built on the same flag/condition logic as
 * cps1_cpu68k.c (cps1_cpu68k_add_flags/sub_flags/cc_test) so the
 * interpreter and any recompiled translation agree on results by
 * construction, not by coincidence.
 */
#include "cps1_cpu68k.h"

/* Dn = Dn (op) data, size-masked merge into the register (0=byte,1=word,
 * 2=long), flags updated in regs->sr. */
void cps1_rc_add(cps1_cpu68k_t *regs, unsigned reg, uint32_t data, unsigned size);
void cps1_rc_sub(cps1_cpu68k_t *regs, unsigned reg, uint32_t data, unsigned size);

/* MOVEQ's flag behavior: N/Z from the sign-extended value, V/C cleared. */
void cps1_rc_set_nz_flags(cps1_cpu68k_t *regs, int32_t value);

/* One per condition code, generated code calls these by name (cps1_rc_cc_T,
 * cps1_rc_cc_EQ, ...) instead of passing a numeric condition around. */
int cps1_rc_cc_T(const cps1_cpu68k_t *regs);
int cps1_rc_cc_F(const cps1_cpu68k_t *regs);
int cps1_rc_cc_HI(const cps1_cpu68k_t *regs);
int cps1_rc_cc_LS(const cps1_cpu68k_t *regs);
int cps1_rc_cc_CC(const cps1_cpu68k_t *regs);
int cps1_rc_cc_CS(const cps1_cpu68k_t *regs);
int cps1_rc_cc_NE(const cps1_cpu68k_t *regs);
int cps1_rc_cc_EQ(const cps1_cpu68k_t *regs);
int cps1_rc_cc_VC(const cps1_cpu68k_t *regs);
int cps1_rc_cc_VS(const cps1_cpu68k_t *regs);
int cps1_rc_cc_PL(const cps1_cpu68k_t *regs);
int cps1_rc_cc_MI(const cps1_cpu68k_t *regs);
int cps1_rc_cc_GE(const cps1_cpu68k_t *regs);
int cps1_rc_cc_LT(const cps1_cpu68k_t *regs);
int cps1_rc_cc_GT(const cps1_cpu68k_t *regs);
int cps1_rc_cc_LE(const cps1_cpu68k_t *regs);

/* Escape hatch for every opcode translate.py doesn't recognize yet: run the
 * real interpreter for one instruction at `addr` instead of guessing. regs
 * must already have `code`/`code_len` set to the same program the
 * recompiled function was generated from (see cps1_core.c). TODO(cps1):
 * once recompiled functions can span multiple ROM regions, this needs a
 * real chained dispatcher, not a same-buffer assumption. */
void cps1_rc_fallback_step(cps1_cpu68k_t *regs, uint32_t addr);
