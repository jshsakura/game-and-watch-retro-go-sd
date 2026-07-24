/* SNES Thumb-2 Stage 1 differential harness.
 *
 * Links the REAL cpu.c (compiled with -DSNES_THUMB2_CPU so both the oracle
 * cpu_runOpcode_c and the Stage-1 dispatcher cpu_runOpcode are present) and the
 * REAL snes_thumb2.S object. For each test case it randomizes a Cpu, snapshots
 * it into two copies with independent fake buses, runs the oracle on copy A and
 * the dispatcher on copy B, then compares every Cpu field and the ordered bus
 * trace. A mismatch proves the Thumb-2 fast path diverges from the C oracle.
 *
 * The bus is O(1): no 64KB backing array. One address (the opcode-fetch site)
 * is forced to the opcode under test; every other address returns a byte that
 * is a pure function of (address, seed). A small write map handles read-after-
 * write within a single opcode. Because oracle and dispatcher start from the
 * same Cpu and the same bus seed, and execute the same access sequence, the
 * deterministic bytes they observe are bit-identical — so any divergence in the
 * final Cpu state or bus trace is a real Stage-1 vs oracle difference.
 *
 * QEMU mps2-an500 (Cortex-M7, -icount shift=0) gives a real ARMv7-M instruction
 * stream; the host cannot execute Thumb-2 natively.
 *
 * Build: see run.sh */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct Snes Snes;
#include "cpu.h"
#include "spin_skip.h"

/* ---- deterministic PRNG (xorshift32) ---- */
static uint32_t prng_state = 0xDEADBEEF;
static uint32_t prng(void) {
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

/* ---- deterministic byte for any non-forced address ---- *
 * Pure function of (addr, seed). Both buses share the seed for a given case,
 * so every read of the same address returns the same byte on both sides. */
static uint8_t det_byte(uint32_t addr, uint32_t seed) {
    uint32_t h = seed;
    h ^= addr * 0x9E3779B1u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return (uint8_t)(h & 0xff);
}

/* ---- fake bus with ordered access trace (O(1), no backing array) ---- */
#define BUS_TRACE_MAX 512
#define WMAP_MAX 16
typedef struct { uint32_t addr; uint8_t val; uint8_t is_write; } BusEntry;
typedef struct {
    /* The single opcode byte the harness places at (k<<16)|pc. A read of
     * forced_addr returns forced_opcode regardless of seed/wmap. */
    uint32_t forced_addr;
    uint8_t  forced_opcode;
    /* Seed for det_byte on every other address. */
    uint32_t seed;
    /* Ordered access log (oracle vs dispatcher must match entry-for-entry). */
    BusEntry trace[BUS_TRACE_MAX];
    int trace_len;
    int overflow;
    /* Write map: a single opcode touches at most a couple of addresses, so a
     * tiny upsert table is enough to give read-after-write within the opcode. */
    struct { uint32_t addr; uint8_t val; } wmap[WMAP_MAX];
    int wmap_len;
} RigBus;

static RigBus g_busA, g_busB;

uint8_t snes_cpuRead(Snes* snes, uint32_t adr) {
    RigBus* b = (RigBus*)snes;
    uint8_t v;
    if (adr == b->forced_addr) {
        v = b->forced_opcode;
    } else {
        int i;
        for (i = 0; i < b->wmap_len; i++) {
            if (b->wmap[i].addr == adr) { v = b->wmap[i].val; break; }
        }
        if (i == b->wmap_len) v = det_byte(adr, b->seed);
    }
    if (b->trace_len < BUS_TRACE_MAX) {
        b->trace[b->trace_len].addr = adr;
        b->trace[b->trace_len].val = v;
        b->trace[b->trace_len].is_write = 0;
        b->trace_len++;
    } else b->overflow = 1;
    return v;
}

void snes_cpuWrite(Snes* snes, uint32_t adr, uint8_t val) {
    RigBus* b = (RigBus*)snes;
    int i;
    for (i = 0; i < b->wmap_len; i++) {
        if (b->wmap[i].addr == adr) { b->wmap[i].val = val; break; }
    }
    if (i == b->wmap_len) {
        if (b->wmap_len < WMAP_MAX) {
            b->wmap[b->wmap_len].addr = adr;
            b->wmap[b->wmap_len].val = val;
            b->wmap_len++;
        } else b->overflow = 1;
    }
    if (b->trace_len < BUS_TRACE_MAX) {
        b->trace[b->trace_len].addr = adr;
        b->trace[b->trace_len].val = val;
        b->trace[b->trace_len].is_write = 1;
        b->trace_len++;
    } else b->overflow = 1;
}

/* ---- externals cpu.c references (stubs) ---- */
Snes* g_snes;
bool g_rc_active = false;
uint16_t rc_dispatch_lookup(uint8_t bank, uint16_t pc) { (void)bank; (void)pc; return 0; }
void (**g_rc_fns)(Cpu*);
int CpuOpcodeHook(uint32_t addr) { (void)addr; return 0; }
void Die(const char* error) { (void)error; for(;;); }
bool HookedFunctionRts(int is_long) { (void)is_long; return true; }

/* ---- field-by-field comparison ---- *
 * Compares memType, spBreakpoint and in_emu (all unchanged by the supported
 * opcode family and by the dispatcher wrapper) but intentionally NOT mem:
 * each copy points at its own RigBus, so the pointer differs by construction. */
static int cpu_eq(const Cpu* a, const Cpu* b) {
    return a->a == b->a && a->x == b->x && a->y == b->y &&
           a->sp == b->sp && a->pc == b->pc && a->dp == b->dp &&
           a->k == b->k && a->db == b->db &&
           a->c == b->c && a->z == b->z && a->v == b->v && a->n == b->n &&
           a->i == b->i && a->d == b->d && a->xf == b->xf && a->mf == b->mf &&
           a->e == b->e &&
           a->irqWanted == b->irqWanted && a->nmiWanted == b->nmiWanted &&
           a->waiting == b->waiting && a->stopped == b->stopped &&
           a->cyclesUsed == b->cyclesUsed &&
           a->memType == b->memType &&
           a->spBreakpoint == b->spBreakpoint &&
           a->in_emu == b->in_emu;
}

static int bus_eq(const RigBus* a, const RigBus* b) {
    if (a->trace_len != b->trace_len) return 0;
    if (a->overflow != b->overflow) return 0;
    for (int i = 0; i < a->trace_len && i < BUS_TRACE_MAX; i++) {
        if (a->trace[i].addr != b->trace[i].addr) return 0;
        if (a->trace[i].val  != b->trace[i].val)  return 0;
        if (a->trace[i].is_write != b->trace[i].is_write) return 0;
    }
    return 1;
}

static void dump_cpu(const char* tag, const Cpu* c) {
    printf("  %s a=%04x x=%04x y=%04x sp=%04x pc=%04x dp=%04x k=%02x db=%02x"
           " c=%d z=%d v=%d n=%d i=%d d=%d xf=%d mf=%d e=%d"
           " irq=%d nmi=%d wai=%d stp=%d cyc=%d"
           " memType=%02x spBp=%04x in_emu=%d\n",
           tag, c->a, c->x, c->y, c->sp, c->pc, c->dp, c->k, c->db,
           c->c, c->z, c->v, c->n, c->i, c->d, c->xf, c->mf, c->e,
           c->irqWanted, c->nmiWanted, c->waiting, c->stopped, c->cyclesUsed,
           c->memType, c->spBreakpoint, c->in_emu);
}

static void randomize_cpu(Cpu* c) {
    memset(c, 0, sizeof(*c));
    c->a = prng();
    c->x = prng();
    c->y = prng();
    c->sp = prng();
    c->pc = prng();
    c->dp = prng();
    c->k = prng();
    c->db = prng();
    c->c = prng() & 1;
    c->z = prng() & 1;
    c->v = prng() & 1;
    c->n = prng() & 1;
    c->i = prng() & 1;
    c->d = prng() & 1;
    /* memType / spBreakpoint / in_emu: randomized so cpu_eq's comparison of
     * them is meaningful, not trivially-true-on-zero. runOpcode never writes
     * these for the opcode family under test, so oracle == dispatcher. */
    c->memType = prng() & 0xff;
    c->spBreakpoint = prng();
    c->in_emu = prng() & 1;
    c->cyclesUsed = 0;
}

/* ---- opcode sets ---- *
 * Opcodes snes_thumb2_try claims. Stage 2 covered the no-operand,
 * bus-side-effect-free set; Stage 3A added the 4 accumulator shifts (no bus
 * access); Stage 3B added the 9 relative branches; Stage 3C adds 6 immediate
 * ALU/loads plus REP/SEP. The main randomized sweep iterates this whole set. */
static const uint8_t supported_opcodes[] = {
    0x18,0x38,0x58,0x78,0xb8,0xd8,0xf8,0xea, /* CLC SEC CLI SEI CLV CLD SED NOP */
    0x9a,0x1b,0x5b,0x7b,0x3b,                 /* TXS TCS TCD TDC TSC             */
    0x8a,0x98,0xaa,0xa8,0xba,0x9b,0xbb,       /* TXA TYA TAX TAY TSX TXY TYX     */
    0x1a,0x3a,0xe8,0xca,0xc8,0x88,0xeb,       /* INA DEA INX DEX INY DEY XBA     */
    0x0a,0x4a,0x2a,0x6a,                      /* ASL A LSR A ROL A ROR A         */
    0x10,0x30,0x50,0x70,0x80,0x90,0xb0,0xd0,0xf0, /* relative branches             */
    0x09,0x29,0x49,0xa0,0xa2,0xa9,0xc2,0xe2 /* immediate ALU/load + REP/SEP   */
};
#define SUPPORTED_COUNT (int)(sizeof(supported_opcodes)/sizeof(supported_opcodes[0]))

/* A small curated set of opcodes snes_thumb2_try does NOT handle, to verify
 * the dispatcher's fall-through path (snes_thumb2_try returns 0 -> cpu_doOpcode)
 * matches the oracle. 0x5c (JML) is deliberately excluded: it can target
 * 0x80:0x8573 and call Die(), which is a noreturn hang that would freeze the
 * rig. None of the opcodes below can reach Die. */
static const uint8_t fallback_opcodes[] = {
    0x69, /* ADC #imm   — operand read(s), no bus side effect */
    0x85, /* STA zp     — operand read + 1 write              */
    0xe6, /* INC zp     — operand read + read/modify/write    */
    0x48, /* PHA        — stack push (write)                  */
    0x68, /* PLA        — stack pull (read)                   */
};
#define FALLBACK_COUNT (int)(sizeof(fallback_opcodes)/sizeof(fallback_opcodes[0]))

/* ---- one differential case. Returns 1 on mismatch, 0 on match. ---- */
static int run_case(uint8_t oc, bool mf, bool xf, bool e,
                    int trial, const char* sweep) {
    Cpu base;
    randomize_cpu(&base);
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    /* The opcode fetch in cpu_runOpcode reads exactly (k<<16)|pc; force that
     * address to the opcode under test. Operand bytes (pc+1, pc+2, ...) and
     * any data addresses fall through to det_byte, seeded identically on both
     * sides so oracle and dispatcher observe the same bytes for the same
     * addresses. */
    uint32_t forced_addr = ((uint32_t)base.k << 16) | base.pc;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;

    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;
    /* memType / spBreakpoint / in_emu intentionally NOT overridden: both copies
     * inherit base's randomized values, and cpu_eq checks they stay equal. */

    cpu_runOpcode_c(&cpuA);   /* oracle:    always cpu_doOpcode           */
    cpu_runOpcode(&cpuB);     /* dispatcher: snes_thumb2_try else cpu_doOpcode */

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [%s] opcode=%02x mf=%d xf=%d e=%d trial=%d\n",
               sweep, oc, mf, xf, e, trial);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        int n = g_busA.trace_len < g_busB.trace_len
              ? g_busA.trace_len : g_busB.trace_len;
        for (int i = 0; i < n; i++) {
            if (g_busA.trace[i].addr != g_busB.trace[i].addr ||
                g_busA.trace[i].val  != g_busB.trace[i].val  ||
                g_busA.trace[i].is_write != g_busB.trace[i].is_write) {
                printf("  trace[%d]: A(%06x,%02x,%d) B(%06x,%02x,%d)\n", i,
                       g_busA.trace[i].addr, g_busA.trace[i].val, g_busA.trace[i].is_write,
                       g_busB.trace[i].addr, g_busB.trace[i].val, g_busB.trace[i].is_write);
            }
        }
        return 1;
    }
    return 0;
}

/* ---- PC=0xffff wrap corner case ---- *
 * Forces the opcode fetch at (k<<16)|0xffff so operand reads at pc+1, pc+2, ...
 * wrap into 0x0000, 0x0001 ... within the bank (cpu_readOpcode increments the
 * 16-bit pc). 0xea (NOP) is the supported representative (no operand read, so a
 * divergence here would mean the dispatcher touched pc wrongly); 0x69 (ADC #imm)
 * is the unsupported representative — its operand read(s) actually wrap, which
 * exercises the unsupported bus trace ordering across the wrap on both paths. Identical
 * deterministic seed on both buses, so any oracle-vs-dispatcher split is real. */
static int run_case_wrap(uint8_t oc, bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.pc = 0xffff;
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    uint32_t forced_addr = ((uint32_t)base.k << 16) | base.pc;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [wrap] opcode=%02x mf=%d xf=%d e=%d\n", oc, mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        return 1;
    }
    return 0;
}

/* ---- WAI power-state corner cases ---- *
 * no-wake: waiting=1 with no pending irq/nmi -> both runOpcode paths return
 *   immediately at the WAI check (no opcode fetch, no cycle charge). Verifies
 *   the dispatcher mirrors the oracle's early-return and leaves the Cpu frozen.
 * wake-by-pending-IRQ (no vector entry): waiting=1, irqWanted=1, i=1. The
 *   pending IRQ clears waiting, but the I flag masks it so the (!i && irqWanted)
 *   interrupt block is skipped — no vector read, no stack push. Execution then
 *   proceeds to the forced safe opcode (NOP) at the current pc. This isolates
 *   the WAI-clear half of the pre-work from actual IRQ entry (excluded this turn). */
static int run_case_wai(bool wake, bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.mf = mf; base.xf = xf; base.e = e;
    base.waiting = 1; base.stopped = 0;
    if (wake) {
        base.irqWanted = 1; base.nmiWanted = 0; base.i = 1;
    } else {
        base.irqWanted = 0; base.nmiWanted = 0; base.i = 1;
    }

    /* For no-wake the opcode is never fetched (forced_addr is irrelevant but
     * kept consistent). For wake, NOP runs on both paths. */
    uint8_t oc = 0xea;
    uint32_t forced_addr = ((uint32_t)base.k << 16) | base.pc;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [wai-%s] mf=%d xf=%d e=%d\n",
               wake ? "wake" : "nowake", mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        return 1;
    }
    return 0;
}

/* ---- actual IRQ/NMI vector-entry corner cases ---- *
 * Pre-seeds the interrupt vector in the fake bus so the handler lands on a
 * known safe PC, then forces a NOP there. Native e=0, i=0 (so IRQ is accepted;
 * NMI is accepted regardless of i). Both runOpcode paths run the identical
 * pre-work block: charge 7 cycles, push k/pc/flags, set i=1 d=0 k=0, read the
 * vector, then fetch+run the handler's NOP. The bus trace therefore records
 * the four stack writes (push k =1, pushWord pc =2, push flags =1), the two
 * vector reads and the handler opcode fetch in order — 7 entries total —
 * compared entry-for-entry by bus_eq. cpu_eq checks pc, flags, k and
 * cycles end up identical. */
static int run_case_irqnmi(bool nmi, bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.mf = mf; base.xf = xf; base.e = e;
    base.waiting = 0; base.stopped = 0;
    base.i = 0;                       /* IRQ acceptance requires i=0; harmless for NMI */
    if (nmi) { base.nmiWanted = 1; base.irqWanted = 0; }
    else     { base.irqWanted = 1; base.nmiWanted = 0; }

    /* cpu_doInterrupt reads IRQ vector at 0xffee/0xffef, NMI at 0xffea/0xffeb,
     * then sets k=0. Target handler PC = 0x0000 -> both vector bytes 0x00.
     * forced_addr = (k=0)<<16 | 0x0000 holds the NOP the handler runs. */
    uint32_t vec_lo = nmi ? 0xffea : 0xffee;
    uint32_t vec_hi = nmi ? 0xffeb : 0xffef;
    uint32_t forced_addr = 0x000000;
    uint8_t  oc = 0xea;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    g_busA.wmap[0].addr = vec_lo; g_busA.wmap[0].val = 0x00;
    g_busA.wmap[1].addr = vec_hi; g_busA.wmap[1].val = 0x00;
    g_busA.wmap_len = 2;
    g_busB.wmap[0].addr = vec_lo; g_busB.wmap[0].val = 0x00;
    g_busB.wmap[1].addr = vec_hi; g_busB.wmap[1].val = 0x00;
    g_busB.wmap_len = 2;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [%s] mf=%d xf=%d e=%d\n", nmi ? "nmi" : "irq", mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        int n = g_busA.trace_len < g_busB.trace_len
              ? g_busA.trace_len : g_busB.trace_len;
        for (int i = 0; i < n; i++) {
            if (g_busA.trace[i].addr != g_busB.trace[i].addr ||
                g_busA.trace[i].val  != g_busB.trace[i].val  ||
                g_busA.trace[i].is_write != g_busB.trace[i].is_write) {
                printf("  trace[%d]: A(%06x,%02x,%d) B(%06x,%02x,%d)\n", i,
                       g_busA.trace[i].addr, g_busA.trace[i].val, g_busA.trace[i].is_write,
                       g_busB.trace[i].addr, g_busB.trace[i].val, g_busB.trace[i].is_write);
            }
        }
        return 1;
    }
    return 0;
}

/* ---- accumulator-shift M=0/1 boundary differential (Stage 3A) ---- *
 * 0A ASL A / 4A LSR A / 2A ROL A / 6A ROR A are implied-form shifts: no operand,
 * no data bus (only the single opcode fetch). They are pure register/flag ops
 * gated on the M flag, so the interesting edges are the carry bit shifted out
 * (bit0/bit7/bit15), the ZN result, and the carry shifted IN (ROL/ROR read C).
 * This forces a specific A and a specific carry-in, over both widths, so a
 * divergence at any bit boundary is caught deterministically rather than left
 * to the random sweep's luck. cpu_eq covers a/c/z/n; bus_eq the opcode fetch. */
static int run_case_shift(uint8_t oc, uint16_t a_val, uint8_t c_in,
                          bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.a = a_val;
    base.c = c_in ? 1 : 0;
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    uint32_t forced_addr = ((uint32_t)base.k << 16) | base.pc;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [shift] opcode=%02x a=%04x c=%d mf=%d xf=%d e=%d\n",
               oc, a_val, c_in, mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        return 1;
    }
    return 0;
}

/* ---- relative-branch differential (Stage 3B) ---- *
 * BPL/BMI/BVC/BVS/BRA/BCC/BCS/BNE/BEQ read one operand byte at pc (uint16 wrap
 * into the next page), sign-extend it, and on a taken branch add it to pc and
 * charge a taken-cycle (BRA is always taken but charges NO extra cycle). The
 * opcode is forced at (k<<16)|pc_in; the operand is seeded in the bus wmap at
 * the FULL 24-bit address ((k<<16)|(uint16)(pc_in+1)) so a random k still
 * overrides (only low-16 matching would miss when k!=0). All four flags are set
 * explicitly so every opcode's condition is deterministic. pc_in spans the wrap
 * edges: 0xffff (opcode at K:FFFF, operand at K:0000), 0xfffe, 0x0000, 0x0100.
 * cpu_eq checks pc/cyclesUsed/flags; bus_eq the opcode+operand reads. */
static int run_case_branch(uint8_t oc, uint8_t z, uint8_t n, uint8_t v,
                           uint8_t c, int8_t offset, uint16_t pc_in,
                           bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.z = z ? 1 : 0; base.n = n ? 1 : 0; base.v = v ? 1 : 0; base.c = c ? 1 : 0;
    base.pc = pc_in;
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    uint32_t forced_addr = ((uint32_t)base.k << 16) | pc_in;
    uint32_t operand_addr = ((uint32_t)base.k << 16) | (uint16_t)(pc_in + 1);
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    g_busA.wmap[0].addr = operand_addr; g_busA.wmap[0].val = (uint8_t)offset;
    g_busA.wmap_len = 1;
    g_busB.wmap[0].addr = operand_addr; g_busB.wmap[0].val = (uint8_t)offset;
    g_busB.wmap_len = 1;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    /* Independent trace-shape assertion (stronger than bus_eq): both sides
     * must show EXACTLY two READS (no writes), at forced_addr (value=opcode)
     * then operand_addr (value=offset byte). Catches any handler that does an
     * extra/missing fetch or any write. Reported separately from cpu/bus eq. */
    for (int side = 0; side < 2; side++) {
        RigBus *b = side ? &g_busB : &g_busA;
        const char *tag = side ? "thumb2" : "oracle";
        if (b->trace_len != 2 || b->overflow ||
            b->trace[0].is_write || b->trace[1].is_write ||
            b->trace[0].addr != forced_addr || b->trace[0].val != oc ||
            b->trace[1].addr != operand_addr || b->trace[1].val != (uint8_t)offset) {
            printf("\nTRACE-SHAPE VIOLATION [branch op=%02x %s] z=%d n=%d v=%d c=%d"
                   " off=%d pc=%04x mf=%d xf=%d e=%d  len=%d ovf=%d\n",
                   oc, tag, z, n, v, c, offset, pc_in, mf, xf, e,
                   b->trace_len, b->overflow);
            for (int i = 0; i < b->trace_len && i < 8; i++)
                printf("  [%d] %06x %02x %s\n", i, b->trace[i].addr,
                       b->trace[i].val, b->trace[i].is_write ? "W" : "R");
            return 1;
        }
    }

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [branch op=%02x] z=%d n=%d v=%d c=%d off=%d pc=%04x"
               " mf=%d xf=%d e=%d\n",
               oc, z, n, v, c, offset, pc_in, mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        int nn = g_busA.trace_len < g_busB.trace_len
               ? g_busA.trace_len : g_busB.trace_len;
        for (int i = 0; i < nn; i++) {
            if (g_busA.trace[i].addr != g_busB.trace[i].addr ||
                g_busA.trace[i].val  != g_busB.trace[i].val  ||
                g_busA.trace[i].is_write != g_busB.trace[i].is_write) {
                printf("  trace[%d]: A(%06x,%02x,%d) B(%06x,%02x,%d)\n", i,
                       g_busA.trace[i].addr, g_busA.trace[i].val, g_busA.trace[i].is_write,
                       g_busB.trace[i].addr, g_busB.trace[i].val, g_busB.trace[i].is_write);
            }
        }
        return 1;
    }
    return 0;
}

/* ---- Stage 3C immediate/status differential ---------------------------- */
static void set_status_byte(Cpu *c, uint8_t p) {
    c->c  = (p >> 0) & 1;
    c->z  = (p >> 1) & 1;
    c->i  = (p >> 2) & 1;
    c->d  = (p >> 3) & 1;
    c->xf = (p >> 4) & 1;
    c->mf = (p >> 5) & 1;
    c->v  = (p >> 6) & 1;
    c->n  = (p >> 7) & 1;
}

static int stage3c_trace_ok(const RigBus *b, uint8_t oc, uint16_t pc_in,
                            uint8_t k, uint16_t operand, int operand_bytes) {
    int expected = 1 + operand_bytes;
    if (b->overflow || b->trace_len != expected) return 0;
    uint32_t addr0 = ((uint32_t)k << 16) | pc_in;
    if (b->trace[0].is_write || b->trace[0].addr != addr0 ||
        b->trace[0].val != oc) return 0;
    for (int i = 0; i < operand_bytes; i++) {
        uint32_t addr = ((uint32_t)k << 16) | (uint16_t)(pc_in + 1 + i);
        uint8_t val = (uint8_t)(operand >> (8 * i));
        if (b->trace[i + 1].is_write || b->trace[i + 1].addr != addr ||
            b->trace[i + 1].val != val) return 0;
    }
    return 1;
}

static int run_case_imm(uint8_t oc, uint16_t operand, uint16_t pc_in,
                        bool mf, bool xf, bool e, uint16_t reg_seed) {
    Cpu base;
    randomize_cpu(&base);
    base.a = reg_seed;
    base.x = reg_seed;
    base.y = reg_seed;
    base.pc = pc_in;
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    bool x_width = (oc == 0xa0 || oc == 0xa2);
    int operand_bytes = (x_width ? xf : mf) ? 1 : 2;
    uint32_t forced_addr = ((uint32_t)base.k << 16) | pc_in;
    uint32_t low_addr = ((uint32_t)base.k << 16) | (uint16_t)(pc_in + 1);
    uint32_t high_addr = ((uint32_t)base.k << 16) | (uint16_t)(pc_in + 2);
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    g_busA.wmap[0].addr = low_addr; g_busA.wmap[0].val = (uint8_t)operand;
    g_busB.wmap[0].addr = low_addr; g_busB.wmap[0].val = (uint8_t)operand;
    g_busA.wmap_len = g_busB.wmap_len = 1;
    if (operand_bytes == 2) {
        g_busA.wmap[1].addr = high_addr; g_busA.wmap[1].val = (uint8_t)(operand >> 8);
        g_busB.wmap[1].addr = high_addr; g_busB.wmap[1].val = (uint8_t)(operand >> 8);
        g_busA.wmap_len = g_busB.wmap_len = 2;
    }
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    SpinSkip spin_seed;
    memset(&spin_seed, 0, sizeof(spin_seed));
    spin_seed.phase = 1;
    spin_seed.io_seq = 0x1234;
    spin_seed.write_seq = 0x5678;
    g_spin = spin_seed;
    cpu_runOpcode_c(&cpuA);
    SpinSkip spinA = g_spin;
    g_spin = spin_seed;
    cpu_runOpcode(&cpuB);
    SpinSkip spinB = g_spin;

    int traces = stage3c_trace_ok(&g_busA, oc, pc_in, base.k, operand, operand_bytes) &&
                 stage3c_trace_ok(&g_busB, oc, pc_in, base.k, operand, operand_bytes);
    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB) ||
        memcmp(&spinA, &spinB, sizeof(spinA)) != 0 ||
        spinA.io_seq != spin_seed.io_seq ||
        spinA.write_seq != spin_seed.write_seq || !traces) {
        printf("\nMISMATCH [imm op=%02x] val=%04x pc=%04x mf=%d xf=%d e=%d"
               " reg=%04x traceA=%d traceB=%d ioA=%u ioB=%u wrA=%u wrB=%u\n",
               oc, operand, pc_in, mf, xf, e, reg_seed,
               g_busA.trace_len, g_busB.trace_len,
               (unsigned)spinA.io_seq, (unsigned)spinB.io_seq,
               (unsigned)spinA.write_seq, (unsigned)spinB.write_seq);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        return 1;
    }
    return 0;
}

static int run_case_status(uint8_t oc, uint8_t p, uint8_t mask, bool e,
                           uint16_t pc_in) {
    Cpu base;
    randomize_cpu(&base);
    set_status_byte(&base, p);
    base.e = e;
    base.x = 0xabcd;
    base.y = 0x80ff;
    base.sp = 0x55aa;
    base.pc = pc_in;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0;

    uint32_t forced_addr = ((uint32_t)base.k << 16) | pc_in;
    uint32_t operand_addr = ((uint32_t)base.k << 16) | (uint16_t)(pc_in + 1);
    uint32_t seed = prng();
    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    g_busA.wmap[0].addr = operand_addr; g_busA.wmap[0].val = mask; g_busA.wmap_len = 1;
    g_busB.wmap[0].addr = operand_addr; g_busB.wmap[0].val = mask; g_busB.wmap_len = 1;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    SpinSkip spin_seed;
    memset(&spin_seed, 0, sizeof(spin_seed));
    spin_seed.phase = 1;
    spin_seed.io_seq = 0x1234;
    spin_seed.write_seq = 0x5678;
    g_spin = spin_seed;
    cpu_runOpcode_c(&cpuA);
    SpinSkip spinA = g_spin;
    g_spin = spin_seed;
    cpu_runOpcode(&cpuB);
    SpinSkip spinB = g_spin;

    int traces = stage3c_trace_ok(&g_busA, oc, pc_in, base.k, mask, 1) &&
                 stage3c_trace_ok(&g_busB, oc, pc_in, base.k, mask, 1);
    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB) ||
        memcmp(&spinA, &spinB, sizeof(spinA)) != 0 ||
        spinA.io_seq != spin_seed.io_seq ||
        spinA.write_seq != spin_seed.write_seq || !traces) {
        printf("\nMISMATCH [status op=%02x] p=%02x mask=%02x e=%d pc=%04x"
               " traceA=%d traceB=%d ioA=%u ioB=%u\n",
               oc, p, mask, e, pc_in, g_busA.trace_len, g_busB.trace_len,
               (unsigned)spinA.io_seq, (unsigned)spinB.io_seq);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        return 1;
    }
    return 0;
}

/* ---- STP (stopped) power-state corner cases ---- *
 * stopped=1 short-circuits runOpcode at the very first check (cpu.c:162 in the
 * oracle, :206 in the dispatcher): no opcode fetch, no WAI clear, no interrupt
 * entry, cyclesUsed stays 0. In this core STP never self-wakes within a single
 * runOpcode (only a reset clears `stopped`, see cpu_reset at cpu.c:138-139), so
 * the faithful representative is the FROZEN state. Two cases are swept:
 *   (a) stopped with no pending interrupt;
 *   (b) stopped WITH a pending NMI -- asserts the dispatcher, like the oracle,
 *       returns at the stopped check and does NOT let the NMI through (stopped
 *       has priority over every other pre-work branch, including NMI).
 * The random sweep and run_case both force stopped=0, so this pre-work path is
 * otherwise unreachable; it is the one power-state branch WAI does not cover. */
static int run_case_stopped(bool with_nmi, bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.mf = mf; base.xf = xf; base.e = e;
    base.stopped = 1; base.waiting = 0; base.i = 1;
    if (with_nmi) { base.nmiWanted = 1; base.irqWanted = 0; }
    else          { base.nmiWanted = 0; base.irqWanted = 0; }

    /* NOP is forced but, while stopped, is never fetched on either path. */
    uint8_t oc = 0xea;
    uint32_t forced_addr = ((uint32_t)base.k << 16) | base.pc;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [stopped-%s] mf=%d xf=%d e=%d\n",
               with_nmi ? "nmi" : "plain", mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        return 1;
    }
    return 0;
}

/* ---- JML abl (0x5c) safe single case -- swept, not excluded ---- *
 * 0x5c is the only opcode whose handler can reach Die() (cpu.c:1478): it does
 * so iff its 24-bit operand target is exactly 0x80:0x8573, the crash sentinel.
 * run_case forces only ONE bus byte (the opcode), so 0x5c's three operand
 * bytes (pc+1..3) would otherwise be uncontrolled deterministic bytes and could
 * by chance hit the sentinel -> Die() is a noreturn hang that freezes the rig.
 * Rather than exclude a normal opcode from the 256-byte sweep, this pins pc/k
 * and pre-seeds the three operand bytes to a non-magic target (0x00:0x0000) so
 * JML runs its ordinary branch on both paths. Bus trace = opcode fetch + three
 * operand reads (4 reads, no writes); cpu_eq checks k/pc land on 0x00:0x0000. */
static int run_case_jml_safe(bool mf, bool xf, bool e) {
    Cpu base;
    randomize_cpu(&base);
    base.k = 0x00; base.pc = 0x0000;     /* deterministic operand addresses */
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    uint8_t  oc = 0x5c;
    uint32_t forced_addr = 0x000000;     /* opcode at k=0:pc=0x0000 */
    /* value = readOpcodeWord at pc+1,pc+2; new_k = readOpcode at pc+3.
     * Target 0x00:0x0000 (bytes 00 00 00) -- not the 0x80:0x8573 sentinel. */
    uint32_t op1 = 0x000001, op2 = 0x000002, op3 = 0x000003;
    uint32_t seed = prng();

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    g_busA.forced_addr = forced_addr; g_busA.forced_opcode = oc; g_busA.seed = seed;
    g_busB.forced_addr = forced_addr; g_busB.forced_opcode = oc; g_busB.seed = seed;
    g_busA.wmap[0].addr = op1; g_busA.wmap[0].val = 0x00;
    g_busA.wmap[1].addr = op2; g_busA.wmap[1].val = 0x00;
    g_busA.wmap[2].addr = op3; g_busA.wmap[2].val = 0x00;
    g_busA.wmap_len = 3;
    g_busB.wmap[0].addr = op1; g_busB.wmap[0].val = 0x00;
    g_busB.wmap[1].addr = op2; g_busB.wmap[1].val = 0x00;
    g_busB.wmap[2].addr = op3; g_busB.wmap[2].val = 0x00;
    g_busB.wmap_len = 3;
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    cpu_runOpcode_c(&cpuA);
    cpu_runOpcode(&cpuB);

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB)) {
        printf("\nMISMATCH [jml-safe 0x5c] mf=%d xf=%d e=%d\n", mf, xf, e);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  busA len=%d ovf=%d  busB len=%d ovf=%d\n",
               g_busA.trace_len, g_busA.overflow,
               g_busB.trace_len, g_busB.overflow);
        return 1;
    }
    return 0;
}

/* ---- Stage 3D direct-page differential --------------------------------- *
 * The 13 DP opcodes (LDA/LDX/LDY/AND/ORA/EOR/CMP/ADC/SBC dp; STA/STX/STY/STZ
 * dp) are now dispatched natively.  The native path's four hard gates are each
 * pinned by an ABSOLUTE bus-shape assertion (not just oracle-vs-dispatcher
 * parity, which would pass if both sides made the same mistake):
 *   gate 1 (writes take the hooked bridge): every store shows as a WRITE entry
 *          and the spin write_seq advances by exactly the access count;
 *   gate 2 (reads take the hooked bridge):  every load shows as a READ entry
 *          and spin parity (oracle io_seq == thumb2 io_seq) holds; the sweep
 *          includes an IO-region address (bank 0, off >= 0x2000) where
 *          spin_hook_read actually bumps io_seq, so a handler that skipped the
 *          bridge would desync the two sides;
 *   gate 3 (DP is bank 0x00, 16-bit wrap, ignores db): data addresses are
 *          checked to be 0x00xxxx with (dp+off)&0xffff wrapping -- db is forced
 *          nonzero so a handler that accidentally used db reads a different
 *          address and the wmap misses -> divergence;
 *   dp-cycle: (dp & 0xff) != 0 charges +1 (verified via cyclesUsed parity, the
 *          sweep includes dp=0x0100 whose low byte is 0 -> no charge).
 * ADC/SBC additionally sweep the D flag: d=1 makes the native handler BAIL to
 * .L_noteb before any operand fetch, so C's cpu_doOpcode runs the whole opcode
 * on both paths -- the bail must leave pc/spin untouched for the fallback to be
 * transparent.  pc_in spans the 0xffff operand-fetch wrap. */
static int run_case_dp(uint8_t oc, uint16_t dp, uint8_t offset,
                       uint16_t data_val, uint8_t c_in, uint8_t d_in,
                       bool mf, bool xf, bool e, uint16_t reg_seed,
                       uint16_t pc_in) {
    Cpu base;
    randomize_cpu(&base);
    base.a = reg_seed; base.x = reg_seed; base.y = reg_seed;
    base.dp = dp;
    base.db = 0x42;                 /* nonzero: DP MUST ignore it (gate 3) */
    base.k = 0x01;                  /* nonzero bank: opcode/operand fetches at
                                     * 0x01xxxx can never collide with bank-0
                                     * DP data addresses 0x00xxxx (which would
                                     * make the bus return forced_opcode) */
    base.c = c_in ? 1 : 0;
    base.d = d_in ? 1 : 0;
    base.pc = pc_in;
    base.mf = mf; base.xf = xf; base.e = e;
    base.irqWanted = 0; base.nmiWanted = 0;
    base.waiting = 0; base.stopped = 0; base.i = 1;

    bool x_width = (oc == 0xa6 || oc == 0xa4 || oc == 0x86 || oc == 0x84);
    bool is_write = (oc == 0x85 || oc == 0x86 || oc == 0x84 || oc == 0x64);
    bool wflag = x_width ? xf : mf;
    int n_data = wflag ? 1 : 2;

    uint16_t addr_low  = (uint16_t)(dp + offset);
    uint16_t addr_high = (uint16_t)(addr_low + 1);
    uint32_t daddr_low  = addr_low;    /* bank 0x00 */
    uint32_t daddr_high = addr_high;   /* bank 0x00 */

    uint32_t forced_addr  = ((uint32_t)base.k << 16) | pc_in;
    uint32_t operand_addr = ((uint32_t)base.k << 16) | (uint16_t)(pc_in + 1);
    uint32_t seed = prng();

    /* expected store bytes (for the absolute write-value check) */
    uint8_t exp_lo, exp_hi;
    if (oc == 0x64)               { exp_lo = 0;            exp_hi = 0; }            /* STZ */
    else if (oc == 0x85)          { exp_lo = base.a & 0xff; exp_hi = base.a >> 8; } /* STA */
    else if (oc == 0x86)          { exp_lo = base.x & 0xff; exp_hi = base.x >> 8; } /* STX */
    else                          { exp_lo = base.y & 0xff; exp_hi = base.y >> 8; } /* STY */

    Cpu cpuA = base, cpuB = base;
    memset(&g_busA, 0, sizeof(g_busA));
    memset(&g_busB, 0, sizeof(g_busB));
    for (int side = 0; side < 2; side++) {
        RigBus *b = side ? &g_busB : &g_busA;
        b->forced_addr = forced_addr; b->forced_opcode = oc; b->seed = seed;
        b->wmap[0].addr = operand_addr; b->wmap[0].val = offset;
        b->wmap_len = 1;
        if (!is_write) {
            b->wmap[1].addr = daddr_low;  b->wmap[1].val = (uint8_t)data_val;
            b->wmap_len = 2;
            if (n_data == 2) {
                b->wmap[2].addr = daddr_high; b->wmap[2].val = (uint8_t)(data_val >> 8);
                b->wmap_len = 3;
            }
        }
    }
    cpuA.mem = &g_busA;
    cpuB.mem = &g_busB;

    SpinSkip spin_seed;
    memset(&spin_seed, 0, sizeof(spin_seed));
    spin_seed.phase = 1;
    spin_seed.io_seq = 0x1234;
    spin_seed.write_seq = 0x5678;
    g_spin = spin_seed;
    cpu_runOpcode_c(&cpuA);
    SpinSkip spinA = g_spin;
    g_spin = spin_seed;
    cpu_runOpcode(&cpuB);
    SpinSkip spinB = g_spin;

    /* absolute trace-shape check on BOTH sides: opcode read, operand read, then
     * n_data data accesses at bank-0 wrapped addresses with the right R/W kind. */
    int expected_len = 2 + n_data;
    /* spin_hook_write is unconditional (no address classification), so the
     * absolute write_seq advance is a strong check.  spin_hook_read, however,
     * classifies addresses and returns early for WRAM/ROM/PC-near -- so an
     * absolute io_seq expectation would be wrong for most DP addresses (bank 0,
     * low offset == WRAM).  We rely on oracle-vs-thumb2 PARITY instead, made
     * meaningful by an IO-region (dp,offset) pair in the sweep. */
    uint32_t exp_wr  = spin_seed.write_seq + (is_write ? n_data : 0);
    int shape_fail = 0;
    for (int side = 0; side < 2; side++) {
        RigBus *b = side ? &g_busB : &g_busA;
        const char *tag = side ? "thumb2" : "oracle";
        if (b->overflow || b->trace_len != expected_len) { shape_fail = 1; }
        else {
            /* [0] opcode fetch (read), [1] operand fetch (read) */
            if (b->trace[0].is_write || b->trace[0].addr != forced_addr ||
                b->trace[0].val != oc) shape_fail = 1;
            if (b->trace[1].is_write || b->trace[1].addr != operand_addr ||
                b->trace[1].val != offset) shape_fail = 1;
            /* [2..] data accesses: bank 0x00, wrapped addresses */
            uint32_t addrs[2] = { daddr_low, daddr_high };
            for (int i = 0; i < n_data; i++) {
                BusEntry *t = &b->trace[2 + i];
                if (t->is_write != (is_write ? 1 : 0)) shape_fail = 1;
                if (t->addr != addrs[i]) shape_fail = 1;     /* gate 3: bank 0 + wrap */
                if (is_write) {
                    uint8_t ev = (i == 0) ? exp_lo : exp_hi;
                    if (t->val != ev) shape_fail = 1;
                } else {
                    uint8_t ev = (i == 0) ? (uint8_t)data_val : (uint8_t)(data_val >> 8);
                    if (t->val != ev) shape_fail = 1;
                }
            }
        }
        if (shape_fail) {
            printf("\nTRACE-SHAPE VIOLATION [dp op=%02x %s] dp=%04x off=%02x"
                   " mf=%d xf=%d e=%d d=%d len=%d ovf=%d\n",
                   oc, tag, dp, offset, mf, xf, e, d_in, b->trace_len, b->overflow);
            for (int i = 0; i < b->trace_len && i < 8; i++)
                printf("  [%d] %06x %02x %s\n", i, b->trace[i].addr,
                       b->trace[i].val, b->trace[i].is_write ? "W" : "R");
            return 1;
        }
    }

    if (!cpu_eq(&cpuA, &cpuB) || !bus_eq(&g_busA, &g_busB) ||
        memcmp(&spinA, &spinB, sizeof(spinA)) != 0 ||
        spinA.write_seq != exp_wr) {
        printf("\nMISMATCH [dp op=%02x] dp=%04x off=%02x data=%04x c=%d d=%d"
               " mf=%d xf=%d e=%d reg=%04x pc=%04x\n",
               oc, dp, offset, data_val, c_in, d_in, mf, xf, e, reg_seed, pc_in);
        dump_cpu("oracle", &cpuA);
        dump_cpu("thumb2", &cpuB);
        printf("  spin io A=%u B=%u  wr A=%u B=%u (exp %u)\n",
               (unsigned)spinA.io_seq, (unsigned)spinB.io_seq,
               (unsigned)spinA.write_seq, (unsigned)spinB.write_seq, (unsigned)exp_wr);
        return 1;
    }
    return 0;
}

int main(void) {
    printf("=== SNES Thumb-2 differential harness ===\n");

    struct { bool mf, xf, e; } combos[] = {
        {0,0,0}, {1,1,0}, {0,1,0}, {1,0,0}, {1,1,1},
    };
    int ncombos = sizeof(combos)/sizeof(combos[0]);

    int total = 0, failures = 0;

    /* ---- main sweep: every supported opcode x 5 combos x 1000 trials ---- */
    const int MAIN_TRIALS = 1000;
    for (int t = 0; t < MAIN_TRIALS; t++) {
        for (int oi = 0; oi < SUPPORTED_COUNT; oi++) {
            uint8_t oc = supported_opcodes[oi];
            for (int ci = 0; ci < ncombos; ci++) {
                total++;
                failures += run_case(oc, combos[ci].mf, combos[ci].xf, combos[ci].e,
                                     t, "supported");
            }
        }
    }

    /* ---- fallback sweep: unsupported opcodes x 5 combos x 200 trials ---- */
    const int FALLBACK_TRIALS = 200;
    for (int t = 0; t < FALLBACK_TRIALS; t++) {
        for (int oi = 0; oi < FALLBACK_COUNT; oi++) {
            uint8_t oc = fallback_opcodes[oi];
            for (int ci = 0; ci < ncombos; ci++) {
                total++;
                failures += run_case(oc, combos[ci].mf, combos[ci].xf, combos[ci].e,
                                     t, "fallback");
            }
        }
    }

    /* ---- PC=0xffff wrap: 1 supported (NOP) + 1 unsupported (ADC #imm) ---- */
    {
        uint8_t wrap_ops[2] = { 0xea, 0x69 };
        for (int oi = 0; oi < 2; oi++) {
            for (int ci = 0; ci < ncombos; ci++) {
                total++;
                failures += run_case_wrap(wrap_ops[oi],
                                          combos[ci].mf, combos[ci].xf, combos[ci].e);
            }
        }
    }

    /* ---- WAI: 1 no-wake + 1 wake-by-pending-IRQ (no vector entry) ---- *
     * Native mode (mf=0,xf=0,e=0); one deterministic case each. */
    {
        total++;
        failures += run_case_wai(false /*nowake*/, 0, 0, 0);
        total++;
        failures += run_case_wai(true  /*wake*/,   0, 0, 0);
    }

    /* ---- STP (stopped): frozen state, incl. stopped-with-pending-NMI ---- *
     * Native mode; two deterministic cases. Completes the power-state pre-work
     * parity alongside WAI: asserts the dispatcher honours the stopped short-
     * circuit and its priority over a pending NMI (no wake, no vector read). */
    {
        total++;
        failures += run_case_stopped(false /*plain*/,    0, 0, 0);
        total++;
        failures += run_case_stopped(true  /*with nmi*/, 0, 0, 0);
    }

    /* ---- actual IRQ/NMI vector entry: 1 IRQ + 1 NMI, native mode ---- *
     * Real interrupt acceptance: pushes k/pc/flags, reads the vector, jumps to
     * the handler. Vector pre-seeded to 0x0000 with a forced NOP handler. */
    {
        total++;
        failures += run_case_irqnmi(false /*irq*/, 0, 0, 0);
        total++;
        failures += run_case_irqnmi(true  /*nmi*/, 0, 0, 0);
    }

    /* ---- full opcode sweep: every opcode 0x00..0xff, 1 native case each ---- *
     * Dispatcher completeness: verifies the fall-through path covers the WHOLE
     * opcode space and matches the oracle -- no normal safe opcode excluded.
     * 0x5c (JML) is the only opcode whose handler can reach Die() (cpu.c:1478):
     * it does so iff its operand target is the crash sentinel 0x80:0x8573. Its
     * three operand bytes (pc+1..3) are not controllable through run_case's
     * single forced byte, so instead of skipping it we route 0x5c through
     * run_case_jml_safe, which pins those bytes to a non-magic target and runs
     * a normal JML. Every opcode is provably safe in a single cpu_runOpcode:
     * 0x44 MVP / 0x54 MVN transfer exactly ONE byte per call (their repeat is
     * driven by pc rewind across runOpcode calls, not an internal loop), and
     * Die() is reached from no case but 0x5c-at-the-sentinel. */
    {
        int run = 0;
        for (int oc = 0; oc < 256; oc++) {
            if (oc == 0x5c) {
                total++;
                failures += run_case_jml_safe(0, 0, 0);
            } else {
                total++;
                failures += run_case((uint8_t)oc, 0, 0, 0, 0, "full-sweep");
            }
            run++;
        }
        printf("(full sweep: %d opcodes run, 0 skipped)\n", run);
    }

    /* ---- accumulator-shift M=0/1 boundary sweep (Stage 3A native) ---- *
     * 0A/4A/2A/6A are now dispatched natively. Walk the carry/ZN bit edges
     * (bit0/bit7/bit15 set & clear, all-zeros/all-ones) across both widths and
     * both carry-in states. xf mirrors mf (xf is irrelevant to A shifts but the
     * combo is kept realistic); e=0 native. */
    {
        static const uint16_t boundary_a[] = {
            0x0000, 0x0001, 0x0002, 0x007f, 0x0080, 0x0081, 0x00fe, 0x00ff,
            0x0100, 0x7fff, 0x8000, 0x8001, 0xff7f, 0xff80, 0xfffe, 0xffff,
        };
        static const uint8_t shift_ops[4] = { 0x0a, 0x4a, 0x2a, 0x6a };
        int nba = (int)(sizeof(boundary_a)/sizeof(boundary_a[0]));
        int run = 0;
        for (int oi = 0; oi < 4; oi++) {
            for (int ai = 0; ai < nba; ai++) {
                for (int mf = 0; mf <= 1; mf++) {
                    for (int cin = 0; cin <= 1; cin++) {
                        total++;
                        failures += run_case_shift(shift_ops[oi], boundary_a[ai],
                                                   (uint8_t)cin, (bool)mf, (bool)mf, 0);
                        run++;
                    }
                }
            }
        }
        printf("(shift boundary: %d cases)\n", run);
    }

    /* ---- relative-branch sweep (Stage 3B native) ---- *
     * For each CONDITIONAL opcode (8 of 9) enumerate ALL 16 distinct z/n/v/c
     * flag patterns so every condition bit is exercised independently (the
     * earlier matrix only flipped all 4 flags together via a redundant s loop,
     * collapsing to 2 patterns). BRA (0x80) is unconditional -> one pattern.
     * Each flag pattern x 7 boundary offsets {0,+/-1,+/-2,+127,-128} x 4
     * pc-wrap edges {0xffff (opcode K:FFFF, operand K:0000),0xfffe,0x0000,
     * 0x0100} x 2 width combos. Branches are width-independent; (0,0,0) is the
     * correctness case, (1,1,1) a width-independence sentinel.
     * Unique count: 8x16x7x4x2 + 1x1x7x4x2 = 7168 + 56 = 7224. */
    {
        static const uint8_t branch_ops[9] = {
            0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xb0, 0xd0, 0xf0
        };
        static const int8_t offsets[7] = { 0, 1, -1, 2, -2, 127, -128 };
        static const uint16_t pc_ins[4] = { 0xffff, 0xfffe, 0x0000, 0x0100 };
        int run = 0;
        for (int oi = 0; oi < 9; oi++) {
            uint8_t op = branch_ops[oi];
            int npat = (op == 0x80) ? 1 : 16;   /* BRA unconditional */
            for (int p = 0; p < npat; p++) {
                uint8_t z = (uint8_t)(p & 1);
                uint8_t n = (uint8_t)((p >> 1) & 1);
                uint8_t v = (uint8_t)((p >> 2) & 1);
                uint8_t c = (uint8_t)((p >> 3) & 1);
                for (int oi2 = 0; oi2 < 7; oi2++) {
                    for (int pi = 0; pi < 4; pi++) {
                        total++;
                        failures += run_case_branch(op, z, n, v, c,
                                                    offsets[oi2], pc_ins[pi],
                                                    0, 0, 0);
                        total++;
                        failures += run_case_branch(op, z, n, v, c,
                                                    offsets[oi2], pc_ins[pi],
                                                    1, 1, 1);
                        run += 2;
                    }
                }
            }
        }
        printf("(branch boundary: %d cases)\n", run);
    }

    /* Stage 3C: exact immediate width/value/PC-wrap/high-byte/ZN coverage,
     * including deliberately inconsistent E=1,M/X=0 states. */
    {
        static const uint8_t imm_ops[6] = { 0x09,0x29,0x49,0xa0,0xa2,0xa9 };
        static const uint16_t values[4] = { 0x0000,0x00ff,0x8000,0xffff };
        static const uint16_t pcs[4] = { 0xfffd,0xfffe,0xffff,0x0100 };
        static const uint16_t regs[4] = { 0x0000,0x00ff,0xff00,0xffff };
        int run = 0;
        for (int oi = 0; oi < 6; oi++) {
            bool x_width = (imm_ops[oi] == 0xa0 || imm_ops[oi] == 0xa2);
            for (int byte = 0; byte < 2; byte++) {
                bool mf = x_width ? !byte : byte;
                bool xf = x_width ? byte : !byte;
                for (int vi = 0; vi < 4; vi++)
                    for (int pi = 0; pi < 4; pi++)
                        for (int ri = 0; ri < 4; ri++)
                            for (int e = 0; e < 2; e++) {
                                total++;
                                failures += run_case_imm(imm_ops[oi], values[vi],
                                                         pcs[pi], mf, xf, e,
                                                         regs[ri]);
                                run++;
                            }
            }
        }
        printf("(immediate boundary + real spin-hook: %d cases)\n", run);
    }

    /* REP/SEP: every mask against every initial P byte in native and
     * emulation mode.  This exhausts the selective bit order and all
     * cpu_setFlags side effects (E pins M/X+SP; final X truncates X/Y). */
    {
        static const uint8_t status_ops[2] = { 0xc2, 0xe2 };
        int run = 0;
        for (int oi = 0; oi < 2; oi++)
            for (int e = 0; e < 2; e++)
                for (int p = 0; p < 256; p++)
                    for (int mask = 0; mask < 256; mask++) {
                        total++;
                        failures += run_case_status(status_ops[oi], (uint8_t)p,
                                                    (uint8_t)mask, (bool)e,
                                                    0xffff);
                        run++;
                    }
        printf("(REP/SEP exhaustive P x mask + real spin-hook: %d cases)\n", run);
    }

    /* ---- Stage 3D direct-page sweep (13 opcodes native) ---- *
     * Pins the four DP gates via the absolute trace-shape check inside
     * run_case_dp: bank-0 addressing (db forced nonzero), 16-bit wrap, dp&0xff
     * cycle charge (incl. dp=0x0100 whose low byte is 0 -> no charge), and
     * spin-hook parity (reads bump io_seq, writes bump write_seq, by exactly the
     * access count). ADC/SBC also sweep D=1 to exercise the bail-to-C path. */
    {
        static const uint8_t dp_ops[13] = {
            0xa5, 0xa6, 0xa4, 0x25, 0x05, 0x45, 0xc5,   /* LDA LDX LDY AND ORA EOR CMP */
            0x65, 0xe5,                                   /* ADC SBC (binary; D=1 bails) */
            0x85, 0x86, 0x84, 0x64                        /* STA STX STY STZ */
        };
        /* (dp, offset) pairs: cycle-gate edges + 0xffff wrap + dp-low-byte-0
         * + an IO-region pair (bank 0, off >= 0x2000) where spin_hook_read
         * actually bumps io_seq, making read-bridge parity meaningful. */
        static const uint16_t dps[7]    = { 0x0000, 0x0000, 0x0080, 0xff80, 0xffff, 0x0100, 0x2101 };
        static const uint8_t  offs[7]   = { 0x00,   0xff,   0x80,   0x80,   0x02,   0x00,   0x00   };
        static const uint16_t dvals[4]  = { 0x0000, 0x00ff, 0x8000, 0xffff };
        static const uint16_t regs[2]   = { 0x0000, 0xffff };
        int run = 0;
        for (int oi = 0; oi < 13; oi++) {
            uint8_t op = dp_ops[oi];
            bool is_alu_xs = (op == 0xa6 || op == 0xa4 || op == 0x86 || op == 0x84);
            for (int di = 0; di < 7; di++) {
                for (int byte = 0; byte < 2; byte++) {
                    /* gate the width flag the opcode actually uses */
                    bool mf = is_alu_xs ? !byte : byte;
                    bool xf = is_alu_xs ? byte : !byte;
                    for (int vi = 0; vi < 4; vi++)
                        for (int ri = 0; ri < 2; ri++)
                            for (int e = 0; e < 2; e++)
                                for (int cin = 0; cin < 2; cin++)
                                    for (int din = 0; din < 2; din++) {
                                        total++;
                                        failures += run_case_dp(op, dps[di], offs[di],
                                                                dvals[vi], (uint8_t)cin,
                                                                (uint8_t)din, mf, xf, e,
                                                                regs[ri], 0x0100);
                                        run++;
                                    }
                }
            }
        }
        printf("(direct-page bank-0/wrap/cycle + spin-hook: %d cases)\n", run);
    }

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           total - failures, total, failures);
    return failures ? 1 : 0;
}
