/* M4A software-mixer HLE — the native block.
 *
 * This is a HAND TRANSLITERATION of M4A's `SoundMainRAM` mixing block: one C
 * statement per ARM instruction, in the original's control flow, with the
 * original's labels kept as C labels named after their ARM addresses, and the
 * original instruction quoted above each line. It is deliberately not "a better
 * mixer" — it is THE SAME mixer, so that its output, its memory writes and its
 * guest cycle count are bit-identical to interpreting it. Read it next to the
 * disassembly; the shape is the proof.
 *
 * Two properties make bit-exactness reachable, and both are worth saying plainly:
 *
 *   - It is all integer arithmetic. There is nothing that could round differently.
 *   - Guest CYCLES are charged, not saved. The block costs the guest exactly what
 *     it always cost, so the game's timeline does not move by a single cycle. We
 *     spend less HOST time to produce the same guest history. (That is the
 *     opposite of the idle-loop skip, which deletes guest cycles on purpose.)
 *
 * The ARM flags are carried explicitly because the block leans on them ACROSS
 * labels — most sharply at L37A8, which is reached both by falling out of a
 * `subs`/`addeq` pair and by a `bgt` from the sample-loop handler, and whose
 * `ldrsbNE` therefore means different things depending on which way you came in.
 * Get that wrong and the sample pointer walks off by one, quietly, in music only.
 */
#include "m4a_hle.h"

#include <string.h>

/* ---------------------------------------------------------------- helpers */

/* A lazily-resolved window into guest memory. gpSP maps in 32 KB pages, so a
 * host pointer is only good to the end of its page; walking off it re-maps.
 * Sample data crosses pages routinely, so this is the common path, not a corner. */
typedef struct {
    const m4a_bus *bus;
    uint8_t *host;      /* host pointer corresponding to guest `lo` */
    uint32_t lo, hi;    /* the guest range [lo, hi) that `host` covers */
    int      c8n, c32n, c32s;
    int      failed;    /* an address would not map: the whole attempt is off */
} m4a_win;

static void win_init(m4a_win *w, const m4a_bus *bus)
{
    memset(w, 0, sizeof *w);
    w->bus = bus;
    w->lo = w->hi = 1;   /* an empty range that cannot match address 0 */
}

static int win_hold(m4a_win *w, uint32_t addr, uint32_t nbytes)
{
    uint32_t span = 0;
    uint8_t *p;

    if (addr >= w->lo && addr + nbytes <= w->hi)
        return 1;
    p = w->bus->map(w->bus->ctx, addr, &span);
    if (!p || span < nbytes) {
        w->failed = 1;
        return 0;
    }
    w->host = p;
    w->lo   = addr;
    w->hi   = addr + span;
    w->bus->cost(w->bus->ctx, addr, &w->c8n, &w->c32n, &w->c32s);
    return 1;
}

static int32_t rd_s8(m4a_win *w, uint32_t addr, int32_t *cyc)
{
    if (!win_hold(w, addr, 1))
        return 0;
    *cyc -= w->c8n;
    return (int32_t)(int8_t)w->host[addr - w->lo];
}

static uint32_t rd_u8(m4a_win *w, uint32_t addr, int32_t *cyc)
{
    if (!win_hold(w, addr, 1))
        return 0;
    *cyc -= w->c8n;
    return w->host[addr - w->lo];
}

static void wr_u8(m4a_win *w, uint32_t addr, uint8_t val, int32_t *cyc)
{
    if (!win_hold(w, addr, 1))
        return;
    *cyc -= w->c8n;
    w->host[addr - w->lo] = val;
}

/* Every 32-bit access the block makes is word-aligned by construction: the mix
 * pointer keeps a 2-bit counter in its TOP bits and only touches memory when
 * that counter is zero (see L3690). A misaligned one would mean our reading of
 * the block is wrong, so refuse rather than invent the rotate the interpreter
 * would have done. Pages are 32 KB-aligned, so an aligned word never straddles
 * one. */
static uint32_t rd_u32(m4a_win *w, uint32_t addr, int32_t *cyc, int seq)
{
    const uint8_t *p;
    if (addr & 3u) { w->failed = 1; return 0; }
    if (!win_hold(w, addr, 4))
        return 0;
    *cyc -= seq ? w->c32s : w->c32n;
    p = w->host + (addr - w->lo);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(m4a_win *w, uint32_t addr, uint32_t val, int32_t *cyc, int seq)
{
    uint8_t *p;
    if (addr & 3u) { w->failed = 1; return; }
    if (!win_hold(w, addr, 4))
        return;
    *cyc -= seq ? w->c32s : w->c32n;
    p = w->host + (addr - w->lo);
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
}

static uint32_t ror32(uint32_t x, uint32_t k)
{
    k &= 31u;
    return k ? ((x >> k) | (x << (32u - k))) : x;
}

/* ARM flag setters, spelled once so that every `subs`/`adds` below sets them the
 * same way. `sub` is a + ~b + 1, so C is "no borrow", i.e. a >= b unsigned. */
#define SET_LOGIC(res)      do { n_f = (res) >> 31; z_f = ((res) == 0); } while (0)
#define SET_SUB(a_, b_, d_) do { n_f = (d_) >> 31; z_f = ((d_) == 0);                 \
                                 c_f = ((a_) >= (b_));                                \
                                 v_f = ((((a_) ^ (b_)) & ((a_) ^ (d_))) >> 31); } while (0)
#define SET_ADD(a_, b_, d_) do { n_f = (d_) >> 31; z_f = ((d_) == 0);                 \
                                 c_f = ((d_) < (a_));                                 \
                                 v_f = ((~((a_) ^ (b_)) & ((a_) ^ (d_))) >> 31); } while (0)
/* `cmp rX, #0` is a subtract of zero: C is always set (no borrow is possible)
 * and V always clear. Spelled separately so the generic macro is not asked to
 * compare an unsigned value against zero, which is a tautology the compiler is
 * right to complain about. */
#define SET_CMP0(a_)        do { n_f = (a_) >> 31; z_f = ((a_) == 0);                 \
                                 c_f = 1u; v_f = 0u; } while (0)
#define COND_GT()  (!z_f && (n_f == v_f))
#define COND_LE()  ( z_f || (n_f != v_f))

/* Where every guest register lives while the block runs, and how it gets back
 * into the caller's state — spelled once, because it happens at every checkpoint
 * as well as at the end, and a register dropped in one place and not the other is
 * exactly the bug nobody finds. */
#define SAVE_REGS()  do {                                                       \
    s->r[0]  = r0;  s->r[1]  = r1;  s->r[2]  = r2;  s->r[3]  = r3;              \
    s->r[4]  = r4;  s->r[5]  = r5;  s->r[6]  = r6;  s->r[8]  = r8;              \
    s->r[9]  = r9;  s->r[10] = sl;  s->r[12] = ip;  s->r[13] = sp;              \
    s->r[14] = lr;                                                              \
    s->n = n_f; s->z = z_f; s->c = c_f; s->v = v_f;                             \
    s->cycles = cyc;                                                            \
} while (0)

/* The checkpoint, after every single guest instruction.
 *
 * The interpreter tests its cycle budget after each instruction and, when it runs
 * out, drops out of its loop to let update_gba() move the video, the timers and
 * the DMA along — then comes back and carries on from the very next instruction.
 * This block is thousands of cycles long and a slice is a scanline at most, so
 * that happens eight or ten times inside ONE call of the mixer.
 *
 * So we do the same, on the same instruction, on the same cycle: hand the state
 * back (including the PC, so that an interrupt raised in there stacks the right
 * return address), let the hardware catch up, and carry on. Running the block
 * atomically instead was tried, and the screen and the audio came out identical
 * while the CLOCK did not — which is a difference no one would see and any game
 * that reads a timer would feel. */
#define CHK(nxt)  do {                                                          \
    if (cyc <= 0) {                                                             \
        int _rc;                                                                \
        if (FAILED()) return M4A_DECLINED;                                      \
        SAVE_REGS();                                                            \
        s->pc = base + (nxt);                                                   \
        _rc = bus->refill(bus->ctx, s);                                         \
        if (_rc != M4A_OK_CONTINUE)                                             \
            return _rc;                                                         \
        cyc = s->cycles;                                                        \
    }                                                                           \
} while (0)

/* ------------------------------------------------------------- the block */

/* The bytes, exactly as they sit in IWRAM. A game whose mixer differs by one
 * instruction is a different program, and gets interpreted. */
static const uint8_t m4a_code_v1_mono[] = {
    0x00,0x80,0x8d,0xe5, 0x0a,0xa0,0xd4,0xe5, 0x0a,0xa8,0xa0,0xe1, 0x01,0x00,0xd4,0xe5,   /* +000 */
    0x08,0x00,0x10,0xe3, 0x3b,0x00,0x00,0x0a, 0x04,0x00,0x52,0xe3, 0x14,0x00,0x00,0xda,   /* +010 */
    0x08,0x20,0x52,0xe0, 0x00,0xe0,0xa0,0xc3, 0x05,0x00,0x00,0xca, 0x08,0xe0,0xa0,0xe1,   /* +020 */
    0x08,0x20,0x82,0xe0, 0x04,0x80,0x42,0xe2, 0x08,0xe0,0x4e,0xe0, 0x03,0x20,0x12,0xe2,   /* +030 */
    0x04,0x20,0xa0,0x03, 0x00,0x60,0x95,0xe5, 0xd1,0x00,0xd3,0xe0, 0x9a,0x00,0x01,0xe0,   /* +040 */
    0xff,0x18,0xc1,0xe3, 0x66,0x64,0x81,0xe0, 0x01,0x51,0x95,0xe2, 0xf9,0xff,0xff,0x3a,   /* +050 */
    0x04,0x60,0x85,0xe4, 0x04,0x80,0x58,0xe2, 0xf5,0xff,0xff,0xca, 0x0e,0x80,0x98,0xe0,   /* +060 */
    0x44,0x00,0x00,0x0a, 0x00,0x60,0x95,0xe5, 0xd1,0x00,0xd3,0xe0, 0x9a,0x00,0x01,0xe0,   /* +070 */
    0xff,0x18,0xc1,0xe3, 0x66,0x64,0x81,0xe0, 0x01,0x20,0x52,0xe2, 0x11,0x00,0x00,0x0a,   /* +080 */
    0x01,0x51,0x95,0xe2, 0xf7,0xff,0xff,0x3a, 0x04,0x60,0x85,0xe4, 0x04,0x80,0x58,0xe2,   /* +090 */
    0xdc,0xff,0xff,0xca, 0x37,0x00,0x00,0xea, 0x18,0x00,0x9d,0xe5, 0x00,0x00,0x50,0xe3,   /* +0a0 */
    0x05,0x00,0x00,0x0a, 0x14,0x30,0x9d,0xe5, 0x00,0x90,0x62,0xe2, 0x02,0x20,0x90,0xe0,   /* +0b0 */
    0x25,0x00,0x00,0xca, 0x00,0x90,0x49,0xe0, 0xfb,0xff,0xff,0xea, 0x10,0x10,0xbd,0xe8,   /* +0c0 */
    0x00,0x20,0xa0,0xe3, 0x03,0x00,0x00,0xea, 0x10,0x20,0x9d,0xe5, 0x00,0x00,0x52,0xe3,   /* +0d0 */
    0x0c,0x30,0x9d,0x15, 0xe9,0xff,0xff,0x1a, 0x00,0x20,0xc4,0xe5, 0x25,0x0f,0xa0,0xe1,   /* +0e0 */
    0x03,0x51,0xc5,0xe3, 0x03,0x00,0x60,0xe2, 0x80,0x01,0xa0,0xe1, 0x76,0x60,0xa0,0xe1,   /* +0f0 */
    0x04,0x60,0x85,0xe4, 0x21,0x00,0x00,0xea, 0x10,0x10,0x2d,0xe9, 0x1c,0xe0,0x94,0xe5,   /* +100 */
    0x20,0x10,0x94,0xe5, 0x9c,0x01,0x04,0xe0, 0xd0,0x00,0xd3,0xe1, 0xd1,0x10,0xf3,0xe1,   /* +110 */
    0x00,0x10,0x41,0xe0, 0x00,0x60,0x95,0xe5, 0x9e,0x01,0x09,0xe0, 0xc9,0x9b,0x80,0xe0,   /* +120 */
    0x9a,0x09,0x0c,0xe0, 0xff,0xc8,0xcc,0xe3, 0x66,0x64,0x8c,0xe0, 0x04,0xe0,0x8e,0xe0,   /* +130 */
    0xae,0x9b,0xb0,0xe1, 0x07,0x00,0x00,0x0a, 0xfe,0xe5,0xce,0xe3, 0x09,0x20,0x52,0xe0,   /* +140 */
    0xd4,0xff,0xff,0xda, 0x01,0x90,0x59,0xe2, 0x01,0x00,0x80,0x00, 0xd9,0x00,0xb3,0x11,   /* +150 */
    0xd1,0x10,0xf3,0xe1, 0x00,0x10,0x41,0xe0, 0x01,0x51,0x95,0xe2, 0xed,0xff,0xff,0x3a,   /* +160 */
    0x04,0x60,0x85,0xe4, 0x04,0x80,0x58,0xe2, 0xe9,0xff,0xff,0xca, 0x01,0x30,0x43,0xe2,   /* +170 */
    0x10,0x10,0xbd,0xe8, 0x1c,0xe0,0x84,0xe5, 0x18,0x20,0x84,0xe5, 0x28,0x30,0x84,0xe5,   /* +180 */
    0x00,0x80,0x9d,0xe5, 0x01,0x00,0x8f,0xe2, 0x10,0xff,0x2f,0xe1,   /* +190 */
};

#define V1_EXIT_OFF  0x194u   /* the `add r0, pc, #1` that bx's back into Thumb */

/* Regions we are willing to WRITE. EWRAM and IWRAM are plain memory. Anywhere
 * else — I/O, palette, VRAM, OAM, cart backup — a store can MEAN something, and
 * the interpreter's write path, not ours, is the one that knows what. */
static int writable_region(uint32_t addr)
{
    uint32_t r = addr >> 24;
    return r == 2u || r == 3u;
}

static int m4a_run_v1_mono(m4a_state *s, const m4a_bus *bus)
{
    /* Guest registers under their ARM names: sl = r10, ip = r12, lr = r14. */
    const uint32_t base = s->pc;   /* the block's entry address, in the guest */
    uint32_t r0, r1, r2, r3, r4, r5, r6, r8, r9, sl, ip, lr, sp;
    uint32_t n_f, z_f, c_f, v_f;
    int32_t  cyc = s->cycles;
    m4a_win  wch, wmix, wsmp, wstk;   /* channel struct, mix buffer, samples, stack */

    r0 = s->r[0];  r1 = s->r[1];  r2 = s->r[2];  r3 = s->r[3];
    r4 = s->r[4];  r5 = s->r[5];  r6 = s->r[6];  r8 = s->r[8];
    r9 = s->r[9];  sl = s->r[10]; ip = s->r[12]; sp = s->r[13];
    lr = s->r[14];
    n_f = s->n; z_f = s->z; c_f = s->c; v_f = s->v;

    /* The mix pointer carries a 2-bit counter in its top bits, and every load and
     * store below assumes that counter is zero — the block only touches memory on
     * a wrap. Were a caller ever to hand us a mid-word pointer, the "addresses"
     * the block forms would not be addresses at all, and what the interpreter
     * makes of them is its business, not ours. Decline. */
    if ((r5 >> 30) != 0u)
        return M4A_DECLINED;
    if (!writable_region(r5) || !writable_region(r4) || !writable_region(sp))
        return M4A_DECLINED;

    win_init(&wch,  bus);
    win_init(&wmix, bus);
    win_init(&wsmp, bus);
    win_init(&wstk, bus);

    /* Probe every window before writing to any of them: the block's very first
     * instruction is a store, so there is no "check as you go" that could still
     * decline cleanly. After this, a failure can only come from an address the
     * probe already accepted — but we still check, and still decline, because a
     * wrong answer is worse than a slow one. */
    if (!win_hold(&wstk, sp - 8u, 40u) || !win_hold(&wch, r4, 44u) ||
        !win_hold(&wmix, r5, 4u)       || !win_hold(&wsmp, r3, 1u))
        return M4A_DECLINED;

#define FAILED()  (wch.failed || wmix.failed || wsmp.failed || wstk.failed)

    /* 300364c: str   r8, [sp]            */ wr_u32(&wstk, sp, r8, &cyc, 0);            cyc -= 1; CHK(0x004u);
    /* 3003650: ldrb  sl, [r4, #10]       */ sl = rd_u8(&wch, r4 + 10u, &cyc);          cyc -= 1; CHK(0x008u);
    /* 3003654: lsl   sl, sl, #16         */ sl <<= 16;                                 cyc -= 1; CHK(0x00cu);
    /* 3003658: ldrb  r0, [r4, #1]        */ r0 = rd_u8(&wch, r4 + 1u, &cyc);           cyc -= 1; CHK(0x010u);
    /* 300365c: tst   r0, #8              */ SET_LOGIC(r0 & 8u);                        cyc -= 1; CHK(0x014u);
    /* 3003660: beq   L3754               */ cyc -= 1; if (z_f) { cyc -= 1; CHK(0x108u); goto L3754; } CHK(0x018u);

L3664:
    /* 3003664: cmp   r2, #4              */ { uint32_t d = r2 - 4u; SET_SUB(r2, 4u, d); } cyc -= 1; CHK(0x01cu);
    /* 3003668: ble   L36C0               */ cyc -= 1; if (COND_LE()) { cyc -= 1; CHK(0x074u); goto L36C0; } CHK(0x020u);
    /* 300366c: subs  r2, r2, r8          */ { uint32_t a = r2, d = a - r8; SET_SUB(a, r8, d); r2 = d; } cyc -= 1; CHK(0x024u);
    /* 3003670: movgt lr, #0              */ cyc -= 1; if (COND_GT()) lr = 0u; CHK(0x028u);
    /* 3003674: bgt   L3690               */ cyc -= 1; if (COND_GT()) { cyc -= 1; CHK(0x044u); goto L3690; } CHK(0x02cu);
    /* 3003678: mov   lr, r8              */ lr = r8;                                   cyc -= 1; CHK(0x030u);
    /* 300367c: add   r2, r2, r8          */ r2 += r8;                                  cyc -= 1; CHK(0x034u);
    /* 3003680: sub   r8, r2, #4          */ r8 = r2 - 4u;                              cyc -= 1; CHK(0x038u);
    /* 3003684: sub   lr, lr, r8          */ lr -= r8;                                  cyc -= 1; CHK(0x03cu);
    /* 3003688: ands  r2, r2, #3          */ r2 &= 3u; SET_LOGIC(r2);                   cyc -= 1; CHK(0x040u);
    /* 300368c: moveq r2, #4              */ cyc -= 1; if (z_f) r2 = 4u; CHK(0x044u);

L3690:
    /* 3003690: ldr   r6, [r5]            */ r6 = rd_u32(&wmix, r5, &cyc, 0);           cyc -= 1; CHK(0x048u);
L3694:
    /* 3003694: ldrsb r0, [r3], #1        */ r0 = (uint32_t)rd_s8(&wsmp, r3, &cyc); r3 += 1u; cyc -= 1; CHK(0x04cu);
    /* 3003698: mul   r1, sl, r0          */ r1 = sl * r0;                              cyc -= 1; CHK(0x050u);
    /* 300369c: bic   r1, r1, #0xff0000   */ r1 &= ~0x00ff0000u;                        cyc -= 1; CHK(0x054u);
#ifdef M4A_SABOTAGE
    /* See the note at the other M4A_SABOTAGE below. */
    r1 ^= 0x01000000u;
#endif
    /* 30036a0: add   r6, r1, r6, ror #8  */ r6 = r1 + ror32(r6, 8);                    cyc -= 1; CHK(0x058u);
    /* 30036a4: adds  r5, r5, #0x40000000 */ { uint32_t a = r5, d = a + 0x40000000u; SET_ADD(a, 0x40000000u, d); r5 = d; } cyc -= 1; CHK(0x05cu);
    /* 30036a8: bcc   L3694               */ cyc -= 1; if (!c_f) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x048u); goto L3694; } CHK(0x060u);
    /* 30036ac: str   r6, [r5], #4        */ wr_u32(&wmix, r5, r6, &cyc, 0); r5 += 4u;  cyc -= 1; CHK(0x064u);
    /* 30036b0: subs  r8, r8, #4          */ { uint32_t a = r8, d = a - 4u; SET_SUB(a, 4u, d); r8 = d; } cyc -= 1; CHK(0x068u);
    /* 30036b4: bgt   L3690               */ cyc -= 1; if (COND_GT()) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x044u); goto L3690; } CHK(0x06cu);
    /* 30036b8: adds  r8, r8, lr          */ { uint32_t a = r8, d = a + lr; SET_ADD(a, lr, d); r8 = d; } cyc -= 1; CHK(0x070u);
    /* 30036bc: beq   L37D4               */ cyc -= 1; if (z_f) { cyc -= 1; CHK(0x188u); goto L37D4; } CHK(0x074u);

L36C0:
    /* 30036c0: ldr   r6, [r5]            */ r6 = rd_u32(&wmix, r5, &cyc, 0);           cyc -= 1; CHK(0x078u);
L36C4:
    /* 30036c4: ldrsb r0, [r3], #1        */ r0 = (uint32_t)rd_s8(&wsmp, r3, &cyc); r3 += 1u; cyc -= 1; CHK(0x07cu);
    /* 30036c8: mul   r1, sl, r0          */ r1 = sl * r0;                              cyc -= 1; CHK(0x080u);
    /* 30036cc: bic   r1, r1, #0xff0000   */ r1 &= ~0x00ff0000u;                        cyc -= 1; CHK(0x084u);
    /* 30036d0: add   r6, r1, r6, ror #8  */ r6 = r1 + ror32(r6, 8);                    cyc -= 1; CHK(0x088u);
    /* 30036d4: subs  r2, r2, #1          */ { uint32_t a = r2, d = a - 1u; SET_SUB(a, 1u, d); r2 = d; } cyc -= 1; CHK(0x08cu);
    /* 30036d8: beq   L3724               */ cyc -= 1; if (z_f) { cyc -= 1; CHK(0x0d8u); goto L3724; } CHK(0x090u);
L36DC:
    /* 30036dc: adds  r5, r5, #0x40000000 */ { uint32_t a = r5, d = a + 0x40000000u; SET_ADD(a, 0x40000000u, d); r5 = d; } cyc -= 1; CHK(0x094u);
    /* 30036e0: bcc   L36C4               */ cyc -= 1; if (!c_f) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x078u); goto L36C4; } CHK(0x098u);
    /* 30036e4: str   r6, [r5], #4        */ wr_u32(&wmix, r5, r6, &cyc, 0); r5 += 4u;  cyc -= 1; CHK(0x09cu);
    /* 30036e8: subs  r8, r8, #4          */ { uint32_t a = r8, d = a - 4u; SET_SUB(a, 4u, d); r8 = d; } cyc -= 1; CHK(0x0a0u);
    /* 30036ec: bgt   L3664               */ cyc -= 1; if (COND_GT()) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x018u); goto L3664; } CHK(0x0a4u);
    /* 30036f0: b     L37D4               */ cyc -= 2; CHK(0x188u); goto L37D4;

L36F4:
    /* The sample ran out. Note that these are read AFTER the push at L3754 moved
     * sp down by 8, so [sp,#24] and [sp,#20] are the caller's +16 and +12 — the
     * very two words the FAST path reads at L3724 as [sp,#16] and [sp,#12], where
     * no push has happened. One contract, reached two ways.
     *
     * `sp` is a live register here and really moves, which it did not in the first
     * version of this file: the offsets were hand-corrected against a fixed sp
     * instead. That reads the same words and is wrong anyway — because when the
     * block gives way mid-push to let the hardware catch up, an interrupt taken in
     * that window stacks itself at whatever sp SAYS. Say the wrong one and the
     * handler writes over the r4 and ip we just pushed, and the mixer comes back
     * to a channel pointer that is now a return address. */
    /* 30036f4: ldr   r0, [sp, #24]       */ r0 = rd_u32(&wstk, sp + 24u, &cyc, 0);     cyc -= 1; CHK(0x0acu);
    /* 30036f8: cmp   r0, #0              */ SET_CMP0(r0); cyc -= 1; CHK(0x0b0u);
    /* 30036fc: beq   L3718               */ cyc -= 1; if (z_f) { cyc -= 1; CHK(0x0ccu); goto L3718; } CHK(0x0b4u);
    /* 3003700: ldr   r3, [sp, #20]       */ r3 = rd_u32(&wstk, sp + 20u, &cyc, 0);     cyc -= 1; CHK(0x0b8u);
    /* 3003704: rsb   r9, r2, #0          */ r9 = 0u - r2;                              cyc -= 1; CHK(0x0bcu);
L3708:
    /* 3003708: adds  r2, r0, r2          */ { uint32_t a = r0, d = a + r2; SET_ADD(a, r2, d); r2 = d; } cyc -= 1; CHK(0x0c0u);
    /* 300370c: bgt   L37A8               */ cyc -= 1; if (COND_GT()) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x15cu); goto L37A8; } CHK(0x0c4u);
    /* 3003710: sub   r9, r9, r0          */ r9 -= r0;                                  cyc -= 1; CHK(0x0c8u);
    /* 3003714: b     L3708               */ cyc -= 2; CHK(0x0bcu); goto L3708;

L3718:
    /* 3003718: pop   {r4, ip}            */ r4 = rd_u32(&wstk, sp, &cyc, 1);
                                             ip = rd_u32(&wstk, sp + 4u, &cyc, 1);
                                             sp += 8u;                                  cyc -= 1; CHK(0x0d0u);
    /* 300371c: mov   r2, #0              */ r2 = 0u;                                   cyc -= 1; CHK(0x0d4u);
    /* 3003720: b     L3734               */ cyc -= 2; CHK(0x0e8u); goto L3734;

L3724:
    /* 3003724: ldr   r2, [sp, #16]       */ r2 = rd_u32(&wstk, sp + 16u, &cyc, 0);     cyc -= 1; CHK(0x0dcu);
    /* 3003728: cmp   r2, #0              */ SET_CMP0(r2); cyc -= 1; CHK(0x0e0u);
    /* 300372c: ldrne r3, [sp, #12]       */ cyc -= 1; if (!z_f) r3 = rd_u32(&wstk, sp + 12u, &cyc, 0); CHK(0x0e4u);
    /* 3003730: bne   L36DC               */ cyc -= 1; if (!z_f) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x090u); goto L36DC; } CHK(0x0e8u);

L3734:
    /* 3003734: strb  r2, [r4]            */ wr_u8(&wch, r4, (uint8_t)r2, &cyc);        cyc -= 1; CHK(0x0ecu);
    /* 3003738: lsr   r0, r5, #30         */ r0 = r5 >> 30;                             cyc -= 1; CHK(0x0f0u);
    /* 300373c: bic   r5, r5, #0xc0000000 */ r5 &= ~0xc0000000u;                        cyc -= 1; CHK(0x0f4u);
    /* 3003740: rsb   r0, r0, #3          */ r0 = 3u - r0;                              cyc -= 1; CHK(0x0f8u);
    /* 3003744: lsl   r0, r0, #3          */ r0 <<= 3;                                  cyc -= 1; CHK(0x0fcu);
    /* 3003748: ror   r6, r6, r0          */ r6 = ror32(r6, r0 & 0xffu);                cyc -= 1; CHK(0x100u);
    /* 300374c: str   r6, [r5], #4        */ wr_u32(&wmix, r5, r6, &cyc, 0); r5 += 4u;  cyc -= 1; CHK(0x104u);
    /* 3003750: b     L37DC               */ cyc -= 2; CHK(0x190u); goto L37DC;

L3754:
    /* 3003754: push  {r4, ip}            */ sp -= 8u;
                                             wr_u32(&wstk, sp, r4, &cyc, 1);
                                             wr_u32(&wstk, sp + 4u, ip, &cyc, 1);       cyc -= 1; CHK(0x10cu);
    /* 3003758: ldr   lr, [r4, #28]       */ lr = rd_u32(&wch, r4 + 28u, &cyc, 0);      cyc -= 1; CHK(0x110u);
    /* 300375c: ldr   r1, [r4, #32]       */ r1 = rd_u32(&wch, r4 + 32u, &cyc, 0);      cyc -= 1; CHK(0x114u);
    /* 3003760: mul   r4, ip, r1          */ r4 = ip * r1;                              cyc -= 1; CHK(0x118u);
    /* 3003764: ldrsb r0, [r3]            */ r0 = (uint32_t)rd_s8(&wsmp, r3, &cyc);     cyc -= 1; CHK(0x11cu);
    /* 3003768: ldrsb r1, [r3, #1]!       */ r3 += 1u; r1 = (uint32_t)rd_s8(&wsmp, r3, &cyc); cyc -= 1; CHK(0x120u);
    /* 300376c: sub   r1, r1, r0          */ r1 -= r0;                                  cyc -= 1; CHK(0x124u);
    if (FAILED()) return M4A_DECLINED;

L3770:
    /* 3003770: ldr   r6, [r5]            */ r6 = rd_u32(&wmix, r5, &cyc, 0);           cyc -= 1; CHK(0x128u);
L3774:
    /* 3003774: mul   r9, lr, r1          */ r9 = lr * r1;                              cyc -= 1; CHK(0x12cu);
    /* 3003778: add   r9, r0, r9, asr #23 */ r9 = r0 + (uint32_t)(((int32_t)r9) >> 23); cyc -= 1; CHK(0x130u);
    /* 300377c: mul   ip, sl, r9          */ ip = sl * r9;                              cyc -= 1; CHK(0x134u);
    /* 3003780: bic   ip, ip, #0xff0000   */ ip &= ~0x00ff0000u;                        cyc -= 1; CHK(0x138u);
#ifdef M4A_SABOTAGE
    /* prove.sh builds this on purpose, to check the verifier can tell. It is the
     * smallest lie the block could tell: one sample, one step quieter. Nothing
     * crashes, no screenshot changes, and no one would hear it. If the verifier's
     * green light does not go red here, the green light means nothing.
     *
     * Two things about it were wrong before they were right, and both are the same
     * mistake the verifier exists to catch:
     *
     *  - It was only in the fast mixing loop. FFTA's channels all resample, so it
     *    never ran, and the RED test "passed". A saboteur that is never executed
     *    proves as little as a test that never fails. It is now in both loops.
     *  - It flipped bit 0. But `sl` is the volume shifted left by 16, so the low
     *    sixteen bits of this product are always zero and clearing one of them
     *    changes nothing at all. The mixer's signal lives in the TOP byte — which
     *    is what bit 24 is. */
    ip ^= 0x01000000u;
#endif
    /* 3003784: add   r6, ip, r6, ror #8  */ r6 = ip + ror32(r6, 8);                    cyc -= 1; CHK(0x13cu);
    /* 3003788: add   lr, lr, r4          */ lr += r4;                                  cyc -= 1; CHK(0x140u);
    /* 300378c: lsrs  r9, lr, #23         */ r9 = lr >> 23; SET_LOGIC(r9); c_f = (lr >> 22) & 1u; cyc -= 1; CHK(0x144u);
    /* 3003790: beq   L37B4               */ cyc -= 1; if (z_f) { cyc -= 1; CHK(0x168u); goto L37B4; } CHK(0x148u);
    /* 3003794: bic   lr, lr, #0x3f800000 */ lr &= ~0x3f800000u;                        cyc -= 1; CHK(0x14cu);
    /* 3003798: subs  r2, r2, r9          */ { uint32_t a = r2, d = a - r9; SET_SUB(a, r9, d); r2 = d; } cyc -= 1; CHK(0x150u);
    /* 300379c: ble   L36F4               */ cyc -= 1; if (COND_LE()) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x0a8u); goto L36F4; } CHK(0x154u);
    /* 30037a0: subs  r9, r9, #1          */ { uint32_t a = r9, d = a - 1u; SET_SUB(a, 1u, d); r9 = d; } cyc -= 1; CHK(0x158u);
    /* 30037a4: addeq r0, r0, r1          */ cyc -= 1; if (z_f) r0 += r1; CHK(0x15cu);
L37A8:
    /* 30037a8: ldrsbne r0, [r3, r9]!     */ if (!z_f) { r3 += r9;
                                                 r0 = (uint32_t)rd_s8(&wsmp, r3, &cyc); }
                                             cyc -= 1; CHK(0x160u);
    /* 30037ac: ldrsb r1, [r3, #1]!       */ r3 += 1u; r1 = (uint32_t)rd_s8(&wsmp, r3, &cyc); cyc -= 1; CHK(0x164u);
    /* 30037b0: sub   r1, r1, r0          */ r1 -= r0;                                  cyc -= 1; CHK(0x168u);
L37B4:
    /* 30037b4: adds  r5, r5, #0x40000000 */ { uint32_t a = r5, d = a + 0x40000000u; SET_ADD(a, 0x40000000u, d); r5 = d; } cyc -= 1; CHK(0x16cu);
    /* 30037b8: bcc   L3774               */ cyc -= 1; if (!c_f) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x128u); goto L3774; } CHK(0x170u);
    /* 30037bc: str   r6, [r5], #4        */ wr_u32(&wmix, r5, r6, &cyc, 0); r5 += 4u;  cyc -= 1; CHK(0x174u);
    /* 30037c0: subs  r8, r8, #4          */ { uint32_t a = r8, d = a - 4u; SET_SUB(a, 4u, d); r8 = d; } cyc -= 1; CHK(0x178u);
    /* 30037c4: bgt   L3770               */ cyc -= 1; if (COND_GT()) { cyc -= 1; if (FAILED()) return M4A_DECLINED; CHK(0x124u); goto L3770; } CHK(0x17cu);
    /* 30037c8: sub   r3, r3, #1          */ r3 -= 1u;                                  cyc -= 1; CHK(0x180u);
    /* 30037cc: pop   {r4, ip}            */ r4 = rd_u32(&wstk, sp, &cyc, 1);
                                             ip = rd_u32(&wstk, sp + 4u, &cyc, 1);
                                             sp += 8u;                                  cyc -= 1; CHK(0x184u);
    /* 30037d0: str   lr, [r4, #28]       */ wr_u32(&wch, r4 + 28u, lr, &cyc, 0);       cyc -= 1; CHK(0x188u);

L37D4:
    /* 30037d4: str   r2, [r4, #24]       */ wr_u32(&wch, r4 + 24u, r2, &cyc, 0);       cyc -= 1; CHK(0x18cu);
    /* 30037d8: str   r3, [r4, #40]       */ wr_u32(&wch, r4 + 40u, r3, &cyc, 0);       cyc -= 1; CHK(0x190u);
L37DC:
    /* 30037dc: ldr   r8, [sp]            */ r8 = rd_u32(&wstk, sp, &cyc, 0);           cyc -= 1; CHK(0x194u);
    /* That last `cyc -= 1` is the end-of-instruction fetch charge, and it belongs
     * here because the block is accounted for ENTIRELY in this function: gpSP
     * resumes at `m4a_resume`, which sits just past its own
     * `cycles_remaining -= ws_cyc_seq[...]`, and the catch-up path does not go
     * through that line at all. Leave it out and the guest gets one cycle free per
     * block — which is not a crash, it is a clock that runs slightly fast for ever.
     * The verifier caught exactly this, and said "delta -1". */

    /* The interpreter resumes at V1_EXIT_OFF and runs the last two instructions
     * itself (`add r0, pc, #1` / `bx r0`), so the ARM-to-Thumb switch stays in the
     * one place that already knows how to do it. r0 is about to be overwritten by
     * that `add`; we still hand back the value the block left, because a
     * transliteration that is only right where someone is looking is not one. */
    if (FAILED())
        return M4A_DECLINED;

    SAVE_REGS();
    return M4A_DONE;

#undef FAILED
}

static const m4a_variant m4a_v1_mono = {
    "m4a-soundmainram-mono",
    m4a_code_v1_mono,
    (uint32_t)sizeof m4a_code_v1_mono,
    V1_EXIT_OFF,
    m4a_run_v1_mono,
};

const m4a_variant *const m4a_variants[] = {
    &m4a_v1_mono,
    0
};

/* ------------------------------------------------------------- discovery */

const m4a_variant *m4a_identify(const uint8_t *code, uint32_t len)
{
    int i;
    for (i = 0; m4a_variants[i]; i++) {
        const m4a_variant *v = m4a_variants[i];
        if (len >= v->size && memcmp(code, v->code, v->size) == 0)
            return v;
    }
    return 0;
}

const m4a_variant *m4a_scan(const uint8_t *mem, uint32_t len, uint32_t base_addr,
                            uint32_t *out_pc)
{
    uint32_t off;
    /* ARM code, word-aligned wherever a game chooses to put it. */
    for (off = 0; off + 4u <= len; off += 4u) {
        const m4a_variant *v = m4a_identify(mem + off, len - off);
        if (v) {
            if (out_pc)
                *out_pc = base_addr + off;
            return v;
        }
    }
    return 0;
}
