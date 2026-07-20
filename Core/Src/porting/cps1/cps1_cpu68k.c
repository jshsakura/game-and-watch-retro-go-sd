#include "cps1_cpu68k.h"

/* ---- fetch ---- */

static uint16_t fetch16(cps1_cpu68k_t *cpu)
{
    uint32_t pc = cpu->pc;
    uint16_t hi = (pc < cpu->code_len) ? cpu->code[pc] : 0;
    uint16_t lo = (pc + 1 < cpu->code_len) ? cpu->code[pc + 1] : 0;
    cpu->pc += 2;
    return (uint16_t)((hi << 8) | lo);
}

/* ---- condition codes (standard 68000 Bcc/DBcc table) ---- */

int cps1_cpu68k_cc_test(uint16_t sr, unsigned cc)
{
    int c = (sr & CPS1_CPU68K_SR_C) != 0;
    int v = (sr & CPS1_CPU68K_SR_V) != 0;
    int z = (sr & CPS1_CPU68K_SR_Z) != 0;
    int n = (sr & CPS1_CPU68K_SR_N) != 0;

    switch (cc) {
    case 0:  return 1;                  /* T  */
    case 1:  return 0;                  /* F  */
    case 2:  return !c && !z;           /* HI */
    case 3:  return c || z;             /* LS */
    case 4:  return !c;                 /* CC */
    case 5:  return c;                  /* CS */
    case 6:  return !z;                 /* NE */
    case 7:  return z;                  /* EQ */
    case 8:  return !v;                 /* VC */
    case 9:  return v;                  /* VS */
    case 10: return !n;                 /* PL */
    case 11: return n;                  /* MI */
    case 12: return n == v;             /* GE */
    case 13: return n != v;             /* LT */
    case 14: return !z && (n == v);     /* GT */
    case 15: return z || (n != v);      /* LE */
    default: return 0;
    }
}

/* ---- size helpers: mask, sign bit, per-width add/sub with real flags ---- */

static uint32_t size_mask(unsigned size)
{
    switch (size) {
    case 0:  return 0xFFu;         /* byte */
    case 1:  return 0xFFFFu;       /* word */
    default: return 0xFFFFFFFFu;   /* long */
    }
}

static uint32_t sign_bit(unsigned size)
{
    switch (size) {
    case 0:  return 0x80u;
    case 1:  return 0x8000u;
    default: return 0x80000000u;
    }
}

/* result = a + b (masked to `size`); updates X/N/Z/V/C in *sr. */
uint32_t cps1_cpu68k_add_flags(uint32_t a, uint32_t b, unsigned size, uint16_t *sr)
{
    uint32_t mask = size_mask(size);
    uint32_t sb = sign_bit(size);
    a &= mask;
    b &= mask;
    uint32_t result = (a + b) & mask;

    int carry = (a + b) > mask;
    int overflow = ((~(a ^ b)) & (a ^ result) & sb) != 0;

    *sr = (uint16_t)(*sr & ~(CPS1_CPU68K_SR_X | CPS1_CPU68K_SR_N | CPS1_CPU68K_SR_Z |
                              CPS1_CPU68K_SR_V | CPS1_CPU68K_SR_C));
    if (carry)             *sr |= (CPS1_CPU68K_SR_C | CPS1_CPU68K_SR_X);
    if (overflow)          *sr |= CPS1_CPU68K_SR_V;
    if (result == 0)       *sr |= CPS1_CPU68K_SR_Z;
    if (result & sb)       *sr |= CPS1_CPU68K_SR_N;
    return result;
}

/* result = a - b (masked to `size`); updates X/N/Z/V/C in *sr. */
uint32_t cps1_cpu68k_sub_flags(uint32_t a, uint32_t b, unsigned size, uint16_t *sr)
{
    uint32_t mask = size_mask(size);
    uint32_t sb = sign_bit(size);
    a &= mask;
    b &= mask;
    uint32_t result = (a - b) & mask;

    int borrow = a < b;
    int overflow = ((a ^ b) & (a ^ result) & sb) != 0;

    *sr = (uint16_t)(*sr & ~(CPS1_CPU68K_SR_X | CPS1_CPU68K_SR_N | CPS1_CPU68K_SR_Z |
                              CPS1_CPU68K_SR_V | CPS1_CPU68K_SR_C));
    if (borrow)            *sr |= (CPS1_CPU68K_SR_C | CPS1_CPU68K_SR_X);
    if (overflow)          *sr |= CPS1_CPU68K_SR_V;
    if (result == 0)       *sr |= CPS1_CPU68K_SR_Z;
    if (result & sb)       *sr |= CPS1_CPU68K_SR_N;
    return result;
}

static void write_dn_sized(cps1_cpu68k_t *cpu, unsigned reg, unsigned size, uint32_t value)
{
    uint32_t mask = size_mask(size);
    cpu->d[reg] = (cpu->d[reg] & ~mask) | (value & mask);
}

static void cps1_cpu68k_unimplemented(cps1_cpu68k_t *cpu, uint16_t opcode)
{
    (void)opcode;
    cpu->illegal_count++;
}

void cps1_cpu68k_reset(cps1_cpu68k_t *cpu, const uint8_t *code, uint32_t code_len)
{
    for (int i = 0; i < 8; i++) { cpu->d[i] = 0; cpu->a[i] = 0; }
    cpu->pc = 0;
    cpu->sr = 0;
    cpu->cycles = 0;
    cpu->illegal_count = 0;
    cpu->halted = 0;
    cpu->code = code;
    cpu->code_len = code_len;
}

uint32_t cps1_cpu68k_step(cps1_cpu68k_t *cpu)
{
    if (cpu->halted)
        return 0;

    uint16_t op = fetch16(cpu);
    uint32_t cycles = 4;

    if (op == 0x4E71) {
        /* NOP */
    } else if (op == 0x4E75) {
        /* RTS -- no call stack modeled yet, so "return" just ends the run. */
        cpu->halted = 1;
        cycles = 16;
    } else if ((op & 0xF100) == 0x7000) {
        /* MOVEQ #imm8,Dn -- sign-extends to 32 bits. */
        unsigned reg = (op >> 9) & 7;
        int32_t imm = (int8_t)(op & 0xFF);
        cpu->d[reg] = (uint32_t)imm;
        cpu->sr = (uint16_t)(cpu->sr & ~(CPS1_CPU68K_SR_N | CPS1_CPU68K_SR_Z |
                                          CPS1_CPU68K_SR_V | CPS1_CPU68K_SR_C));
        if (imm == 0) cpu->sr |= CPS1_CPU68K_SR_Z;
        if (imm < 0)  cpu->sr |= CPS1_CPU68K_SR_N;
    } else if ((op & 0xF038) == 0x5000 && ((op >> 6) & 3) != 3) {
        /* ADDQ/SUBQ #imm(1-8),Dn -- Dn-direct destination only (ea mode 000,
         * i.e. bits5-3==000; bits7-6==11 is the Scc/DBcc family, excluded). */
        unsigned data = (op >> 9) & 7;
        if (data == 0) data = 8;
        unsigned is_sub = (op >> 8) & 1;
        unsigned size = (op >> 6) & 3;
        unsigned reg = op & 7;
        uint32_t dn = cpu->d[reg];
        uint32_t result = is_sub ? cps1_cpu68k_sub_flags(dn, data, size, &cpu->sr)
                                  : cps1_cpu68k_add_flags(dn, data, size, &cpu->sr);
        write_dn_sized(cpu, reg, size, result);
    } else if ((op & 0xF0F8) == 0x50C8) {
        /* DBcc Dn,disp16 */
        unsigned cc = (op >> 8) & 0xF;
        unsigned reg = op & 7;
        uint32_t ext_addr = cpu->pc; /* address of the displacement word */
        int16_t disp = (int16_t)fetch16(cpu);
        cycles = 10;
        if (!cps1_cpu68k_cc_test(cpu->sr, cc)) {
            uint16_t lo = (uint16_t)(cpu->d[reg] & 0xFFFF) - 1;
            cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000u) | lo;
            if (lo != 0xFFFFu) {
                cpu->pc = (uint32_t)((int32_t)ext_addr + disp);
                cycles = 10;
            } else {
                cycles = 14; /* counter expired, fall through */
            }
        }
        /* condition true: falls through past the extension word already
         * consumed by fetch16() above -- counter untouched. */
    } else if ((op & 0xF000) == 0x6000) {
        /* Bcc/BRA/BSR, 8-bit displacement only (skeleton: no 16/32-bit form). */
        unsigned cc = (op >> 8) & 0xF;
        int8_t disp8 = (int8_t)(op & 0xFF);
        uint32_t branch_base = cpu->pc; /* address right after the opcode word */
        cycles = 10;
        if (cc == 1) {
            /* BSR: push return address -- no call-stack model yet, so treat
             * as unconditional branch (TODO once a stack/memory bus exists). */
            cpu->pc = (uint32_t)((int32_t)branch_base + disp8);
        } else if (cps1_cpu68k_cc_test(cpu->sr, cc)) {
            cpu->pc = (uint32_t)((int32_t)branch_base + disp8);
        }
    } else {
        cps1_cpu68k_unimplemented(cpu, op);
    }

    cpu->cycles += cycles;
    return cycles;
}

uint32_t cps1_cpu68k_run(cps1_cpu68k_t *cpu, uint32_t max_instructions)
{
    uint32_t executed = 0;
    while (executed < max_instructions && !cpu->halted) {
        cps1_cpu68k_step(cpu);
        executed++;
    }
    return executed;
}

uint32_t cps1_cpu68k_state_hash(const cps1_cpu68k_t *cpu)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) { h ^= cpu->d[i]; h *= 16777619u; }
    for (int i = 0; i < 8; i++) { h ^= cpu->a[i]; h *= 16777619u; }
    h ^= cpu->pc;    h *= 16777619u;
    h ^= cpu->sr;    h *= 16777619u;
    h ^= cpu->cycles; h *= 16777619u;
    return h;
}
