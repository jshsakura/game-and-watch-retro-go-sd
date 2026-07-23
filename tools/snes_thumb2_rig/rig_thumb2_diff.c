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
 * Opcodes snes_thumb2_try claims. Stage 2 covered the no-operand,
 * bus-side-effect-free set; Stage 3A added the 4 accumulator shifts (no bus
 * access); Stage 3B added the 9 relative branches (one raw operand fetch, no
 * data-bus write). The main randomized sweep iterates this whole set. */
static const uint8_t supported_opcodes[] = {
    0x18,0x38,0x58,0x78,0xb8,0xd8,0xf8,0xea, /* CLC SEC CLI SEI CLV CLD SED NOP */
    0x9a,0x1b,0x5b,0x7b,0x3b,                 /* TXS TCS TCD TDC TSC             */
    0x8a,0x98,0xaa,0xa8,0xba,0x9b,0xbb,       /* TXA TYA TAX TAY TSX TXY TYX     */
    0x1a,0x3a,0xe8,0xca,0xc8,0x88,0xeb,       /* INA DEA INX DEX INY DEY XBA     */
    0x0a,0x4a,0x2a,0x6a,                      /* ASL A LSR A ROL A ROR A         */
    0x10,0x30,0x50,0x70,0x80,0x90,0xb0,0xd0,0xf0 /* BPL BMI BVC BVS BRA BCC BCS BNE BEQ */
};
#define SUPPORTED_COUNT (int)(sizeof(supported_opcodes)/sizeof(supported_opcodes[0]))

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

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           total - failures, total, failures);
    return failures ? 1 : 0;
}
