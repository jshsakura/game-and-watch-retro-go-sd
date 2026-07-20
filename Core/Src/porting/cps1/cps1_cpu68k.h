#pragma once
/*
 * Minimal freestanding 68000 interpreter core -- skeleton.
 *
 * Covers only the opcodes needed to fetch/decode/execute a real instruction
 * stream end to end: NOP, MOVEQ, ADDQ/SUBQ (Dn direct), Bcc/BRA/BSR (8-bit
 * displacement), DBcc, RTS. Every other opcode is UNIMPLEMENTED and steps
 * through cps1_cpu68k_unimplemented() instead of crashing, so a real
 * instruction set can be filled in incrementally without re-architecting
 * the fetch/step loop. No addressing modes beyond Dn-direct and PC-relative
 * branches yet -- no memory bus, no An-indirect, no absolute addressing.
 * See docs/CPS1_ULTIMATE_PORTING_PLAN.md technique 1/2 for what replaces
 * this next (static recompiler, real addressing modes, WRAM/ROM bus).
 */
#include <stdint.h>

#define CPS1_CPU68K_SR_C 0x0001u
#define CPS1_CPU68K_SR_V 0x0002u
#define CPS1_CPU68K_SR_Z 0x0004u
#define CPS1_CPU68K_SR_N 0x0008u
#define CPS1_CPU68K_SR_X 0x0010u

typedef struct {
    uint32_t d[8];
    uint32_t a[8]; /* a[7] is the active stack pointer for this skeleton */
    uint32_t pc;   /* byte offset into `code` */
    uint16_t sr;
    uint32_t cycles;
    uint32_t illegal_count; /* opcodes seen but not implemented */
    int halted;             /* set by RTS (no call stack yet -- see header) */
    const uint8_t *code;
    uint32_t code_len;
} cps1_cpu68k_t;

void cps1_cpu68k_reset(cps1_cpu68k_t *cpu, const uint8_t *code, uint32_t code_len);

/* Executes exactly one instruction (or does nothing if halted). Returns the
 * cycle count charged for that instruction (0 once halted). */
uint32_t cps1_cpu68k_step(cps1_cpu68k_t *cpu);

/* Steps until halted or max_instructions executed, whichever comes first.
 * Returns the number of instructions actually executed. */
uint32_t cps1_cpu68k_run(cps1_cpu68k_t *cpu, uint32_t max_instructions);

/* FNV-1a over the visible register file -- cheap, deterministic state check
 * for harness diffing (not a savestate format). */
uint32_t cps1_cpu68k_state_hash(const cps1_cpu68k_t *cpu);

/* Shared with tools/cps1_recompiler/translate.py's emitted C (via
 * cps1_rc_runtime.c) so the interpreter and the recompiled output compute
 * identical flags/conditions from one implementation -- not two that can
 * drift apart. size: 0=byte, 1=word, 2=long. */
uint32_t cps1_cpu68k_add_flags(uint32_t a, uint32_t b, unsigned size, uint16_t *sr);
uint32_t cps1_cpu68k_sub_flags(uint32_t a, uint32_t b, unsigned size, uint16_t *sr);
int cps1_cpu68k_cc_test(uint16_t sr, unsigned cc);
