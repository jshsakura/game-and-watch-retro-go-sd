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
 * The 27 no-operand, bus-side-effect-free opcodes snes_thumb2_try claims. */
static const uint8_t supported_opcodes[27] = {
    0x18,0x38,0x58,0x78,0xb8,0xd8,0xf8,0xea, /* CLC SEC CLI SEI CLV CLD SED NOP */
    0x9a,0x1b,0x5b,0x7b,0x3b,                 /* TXS TCS TCD TDC TSC             */
    0x8a,0x98,0xaa,0xa8,0xba,0x9b,0xbb,       /* TXA TYA TAX TAY TSX TXY TYX     */
    0x1a,0x3a,0xe8,0xca,0xc8,0x88,0xeb        /* INA DEA INX DEX INY DEY XBA     */
};

/* A small curated set of opcodes snes_thumb2_try does NOT handle, to verify
 * the dispatcher's fall-through path (snes_thumb2_try returns 0 -> cpu_doOpcode)
 * matches the oracle. 0x5c (JML) is deliberately excluded: it can target
 * 0x80:0x8573 and call Die(), which is a noreturn hang that would freeze the
 * rig. None of the opcodes below can reach Die. */
static const uint8_t fallback_opcodes[] = {
    0xa9, /* LDA #imm   — operand read(s), no bus side effect */
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
 * divergence here would mean the dispatcher touched pc wrongly); 0xa9 (LDA #imm)
 * is the unsupported representative — its operand read(s) actually wrap, which
 * exercises the bus trace ordering across the wrap on both paths. Identical
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

int main(void) {
    printf("=== SNES Thumb-2 Stage 2 differential harness ===\n");

    struct { bool mf, xf, e; } combos[] = {
        {0,0,0}, {1,1,0}, {0,1,0}, {1,0,0}, {1,1,1},
    };
    int ncombos = sizeof(combos)/sizeof(combos[0]);

    int total = 0, failures = 0;

    /* ---- main sweep: 27 supported opcodes x 5 combos x 1000 trials ---- */
    const int MAIN_TRIALS = 1000;
    for (int t = 0; t < MAIN_TRIALS; t++) {
        for (int oi = 0; oi < 27; oi++) {
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

    /* ---- PC=0xffff wrap: 1 supported (NOP) + 1 unsupported (LDA #imm) ---- */
    {
        uint8_t wrap_ops[2] = { 0xea, 0xa9 };
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
     * Dispatcher completeness: verifies the fall-through path covers the whole
     * opcode space and matches the oracle. 0x5c (JML) is the ONLY exclusion:
     * its 3 operand bytes (pc+1..3) are not controllable through run_case's
     * single forced byte, and a JML targeting 0x80:0x8573 calls Die() (a
     * noreturn hang). Every other opcode is provably safe in a single
     * cpu_runOpcode: 0x44 MVP / 0x54 MVN transfer exactly ONE byte per call
     * (their repeat is driven by pc rewind across runOpcode calls, not an
     * internal loop), and Die() is reached from no case but 0x5c (cpu.c:1478). */
    {
        int skipped = 0;
        for (int oc = 0; oc < 256; oc++) {
            if (oc == 0x5c) { skipped++; continue; }
            total++;
            failures += run_case((uint8_t)oc, 0, 0, 0, 0, "full-sweep");
        }
        printf("(full sweep: %d opcodes run, %d skipped)\n", 256 - skipped, skipped);
    }

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           total - failures, total, failures);
    return failures ? 1 : 0;
}
