/* scd_hist.c — 32X-style opcode+PC histogram for the SegaCD dual-68K.
 *
 * Same shape as the 32X Phase-1.7 probe (see memory sega32x-feasibility.md):
 *   g_op_hist_*[65536]   per-opcode counts (one per CPU)
 *   g_pc_hist_*[1 << 21] per-PC counts, low 2 MB of address space (one per CPU)
 *
 * Sampling point = gwenesis Musashi dispatch loop, reached via the HOOK_CPU
 * path: the cpu_hook function pointer is called with HOOK_M68K_E and REG_PC
 * just before each instruction is decoded. The opcode word is fetched from PC
 * by the hook itself (the loop has not set REG_IR yet at the hook site).
 *
 * Lifecycle:
 *   - scd_hist_clear()  scheduled at frame 120 (called from boot_test main loop)
 *   - clear happens lazily on the NEXT hook tick after scheduling (avoids a
 *     partial clear if the clear call lands between main and sub work)
 *   - scd_hist_dump()   top-20 opcode + top-20 PC for each CPU, called at exit
 *
 * Both histograms stay zero cost on the device: this file is harness-only and
 * the entire HOOK_CPU path compiles out when HOOK_CPU is not on the command
 * line (the firmware build does not define it).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "m68k.h"
#include "cpuhook.h"

#define HIST_PC_BITS 21
#define HIST_PC_SIZE (1u << HIST_PC_BITS)
#define HIST_PC_MASK (HIST_PC_SIZE - 1u)
#define HIST_OP_SIZE 65536u
#define TOP_N        20u
#define TOP_N_PC     100u   /* extended PC dump for routine clustering */

#ifndef SCD_CACHE_ONLY
uint32_t g_op_hist_main[HIST_OP_SIZE];
uint32_t g_op_hist_sub [HIST_OP_SIZE];
uint32_t g_pc_hist_main[HIST_PC_SIZE];
uint32_t g_pc_hist_sub [HIST_PC_SIZE];
#endif

/* set by segacd_engine.c when the sub-68K context is loaded into `m68k` */
int g_scd_hist_issub = 0;

/* frame counter — set by boot_test.c each frame for trace correlation */
int g_scd_frame = 0;

/* Total sub-68K instruction count (all frames, for A/B comparison) */
uint64_t g_sub_insn_count = 0;

/* $7c80 rotation-calc input trace — capture stamp descriptor struct at A5
 * to check if coefficients change frame-to-frame (caching viability). */
#define ROT_TRACE_MAX 256
struct rot_entry { int frame; unsigned int a5; unsigned int data[16]; };
static struct rot_entry s_rot_trace[ROT_TRACE_MAX];
static int s_rot_trace_n = 0;

/* 0 = ignore samples (warmup), 1 = record */
static int s_armed = 0;
/* 1 = clear histograms on the next hook tick, then arm */
static int s_clear_pending = 0;
/* 1 = disable after dump so a long tail doesn't keep mutating state */
static int s_frozen = 0;

/* ---- $7c80 rotation coefficient cache (SCD_CACHE) ----
 *
 * ARCHITECTURE: delta-based caching.
 *
 * $7c80 is NOT idempotent — A5+$40 and A5+$64 are read-modify-write
 * accumulators that increment by fixed deltas every call.  But the deltas
 * are a pure function of the input parameters (A5+$00-$3F), which are
 * identical for all stamp calls within a frame and across 2/3 of frames.
 *
 * Key:   64 bytes at A5+$00-$3F (16 longwords — transform parameters)
 * Value: 64 bytes of deltas at A5+$40-$7F (16 longwords — after minus before)
 *
 * On MISS: save inputs to key, save A5+$40-$7F to "before", let routine run.
 *          At RTS: compute delta = after - before, store in cache.
 * On HIT:  read current A5+$40-$7F, add cached deltas, write back, skip routine.
 *
 * Registers: $7c80 preserves D2-D7/A2-A6 (68K callee-saved).  The caller
 * ($7b00) only depends on preserved registers + A5/A1/D2/D6 which are all
 * callee-saved.  D0-D1/A0-A1 are scratch; $7cd0 reinitialises its own from
 * the A5 struct.  So skipping $7c80 leaves the caller's register state valid.
 */
#ifdef SCD_CACHE
static uint32_t s_cache_key[16];    /* input params A5+$00-$3F (64 bytes) */
static uint32_t s_cache_delta[16];  /* deltas A5+$40-$7F: after - before */
static uint32_t s_cache_before[16]; /* A5+$40-$7F snapshot before routine */
static int      s_cache_valid = 0;
static int      s_cache_pending = 0;
static uint32_t s_cache_a5 = 0;
static uint32_t s_cache_hits = 0;
static uint32_t s_cache_misses = 0;
#ifdef SCD_CACHE_VERIFY
static int       s_cache_verify_mode = 0;   /* set on HIT: let routine run, compare at RTS */
static uint32_t  s_cache_verify_a5 = 0;
static uint32_t  s_cache_verify_pre[16]; /* A5+$40 before apply */
static uint32_t  s_cache_verify_mismatches = 0;
static uint32_t  s_cache_verify_checks = 0;
static int       s_cache_verify_cycles0 = 0;  /* cycle cost measurement */
static int       s_cache_verify_cycle_samples = 0;
static int64_t   s_cache_verify_cycle_sum = 0;
#endif

/* Write 32 bits to sub-68K memory via memory_map (callback or base ptr). */
static void scd_write32_direct(uint32_t addr, uint32_t val)
{
    cpu_memory_map *m = &m68k.memory_map[(addr >> 16) & 0xFF];
    if (m->write16) {
        (*m->write16)(addr & 0xFFFFFF, (val >> 16) & 0xFFFF);
        (*m->write16)((addr + 2) & 0xFFFFFF, val & 0xFFFF);
    } else {
        unsigned char *p = m->base + (addr & 0xFFFF);
        p[0] = (val >> 24) & 0xFF;
        p[1] = (val >> 16) & 0xFF;
        p[2] = (val >>  8) & 0xFF;
        p[3] =  val        & 0xFF;
    }
}

void scd_cache_stats(uint32_t *hits, uint32_t *misses)
{
    *hits = s_cache_hits;
    *misses = s_cache_misses;
}

/* Toggle cpu_hook: enable for sub-68K, disable (NULL) for main to eliminate
 * per-instruction overhead on the ~10M main instructions over 900 frames. */
static void scd_hist_hook(int type, int size, unsigned int addr, unsigned int val);

void scd_cache_hook_enable(int enable)
{
    cpu_hook = enable ? scd_hist_hook : NULL;
}
#endif /* SCD_CACHE */

extern unsigned int m68k_read_disassembler_16(unsigned int addr);  /* gwenesis bus */

uint32_t scd_sub_max_addr = 0;       /* max sub-68K data access address */
uint32_t scd_sub_max_addr_frame = 0; /* frame at which max was seen */
uint32_t scd_sub_max_prg_addr = 0;   /* max sub-68K access addr in PRG-RAM (< 0x080000) */
uint32_t scd_sub_max_prg_frame = 0;  /* frame at which PRG max was seen */

static void scd_hist_hook(int type, int size, unsigned int addr, unsigned int val)
{
    (void)size; (void)val;

    /* Track sub-68K data access range (for PRG-RAM sizing decision) */
    if ((type == HOOK_M68K_R || type == HOOK_M68K_W) && g_scd_hist_issub) {
        if (addr > scd_sub_max_addr) {
            scd_sub_max_addr = addr;
            scd_sub_max_addr_frame = (uint32_t)g_scd_frame;
        }
        /* PRG-RAM is $000000-$07FFFF. Filter out GA regs ($FF8000+), Word-RAM
         * ($080000-$0BFFFF), etc. This tells us the highest PRG-RAM byte the
         * sub ever touches → determines minimum PRG-RAM size needed. */
        if (addr < 0x080000 && addr > scd_sub_max_prg_addr) {
            scd_sub_max_prg_addr = addr;
            scd_sub_max_prg_frame = (uint32_t)g_scd_frame;
        }
        return;
    }

    if (type != HOOK_M68K_E) return;
    unsigned int pc = addr;

    if (g_scd_hist_issub) g_sub_insn_count++;

#ifdef SCD_CACHE
    /* $7c80 rotation coefficient cache — sub-68K only */
    if (g_scd_hist_issub) {
        if (pc == 0x7c80) {
            /* Entry to rotation-calc routine */
            unsigned int a5 = m68k_get_reg(M68K_REG_A5);
            uint32_t inputs[16];
            int match = s_cache_valid;
            for (int i = 0; i < 16; i++) {
                inputs[i] = m68k_read_disassembler_32(a5 + (unsigned)(i * 4));
                if (match && inputs[i] != s_cache_key[i]) match = 0;
            }
            if (match) {
#ifdef SCD_CACHE_VERIFY
                /* VERIFY: apply deltas, let routine run, compare at RTS */
                s_cache_verify_mode = 1;
                s_cache_verify_a5 = a5;
                for (int i = 0; i < 16; i++) {
                    s_cache_verify_pre[i] = m68k_read_disassembler_32(
                        a5 + 0x40 + (unsigned)(i * 4));
                }
                /* DON'T skip — let routine run to see what it produces */
                s_cache_hits++;
                /* Fall through to MISS-save so key stays current */
#else
                /* HIT: apply cached deltas to current accumulators */
                for (int i = 0; i < 16; i++) {
                    uint32_t cur = m68k_read_disassembler_32(
                        a5 + 0x40 + (unsigned)(i * 4));
                    scd_write32_direct(a5 + 0x40 + (unsigned)(i * 4),
                                       cur + s_cache_delta[i]);
                }
                /* Skip routine: pop return address, advance cycles */
                unsigned int sp = m68k_get_reg(M68K_REG_A7);
                unsigned int ret = m68k_read_disassembler_32(sp);
                m68k_set_reg(M68K_REG_A7, sp + 4);
                m68k_set_reg(M68K_REG_PC, ret);
                m68k.cycles += 800;
                s_cache_hits++;
                return;
#endif
            }
            /* MISS: save input key + before-snapshot, let routine run */
            memcpy(s_cache_key, inputs, sizeof(s_cache_key));
            s_cache_a5 = a5;
            for (int i = 0; i < 16; i++)
                s_cache_before[i] = m68k_read_disassembler_32(
                    a5 + 0x40 + (unsigned)(i * 4));
            s_cache_pending = 1;
            s_cache_misses++;
#ifdef SCD_CACHE_VERIFY
            /* Track real routine cycle cost for tuning the skip advance */
            s_cache_verify_cycles0 = m68k.cycles;
#endif
        } else if (s_cache_pending && pc == 0x7cce) {
            /* RTS of $7c80 — compute deltas (after - before) */
            for (int i = 0; i < 16; i++) {
                uint32_t after = m68k_read_disassembler_32(
                    s_cache_a5 + 0x40 + (unsigned)(i * 4));
                s_cache_delta[i] = after - s_cache_before[i];
            }
            s_cache_valid = 1;
            s_cache_pending = 0;
        }
#ifdef SCD_CACHE_VERIFY
        if (s_cache_verify_mode && pc == 0x7cce) {
            /* Routine just ran — compare real output with delta-applied result */
            s_cache_verify_mode = 0;
            s_cache_verify_checks++;
            int real_cycles = m68k.cycles - s_cache_verify_cycles0;
            s_cache_verify_cycle_samples++;
            s_cache_verify_cycle_sum += real_cycles;
            if (s_cache_verify_checks <= 5)
                fprintf(stderr, "[VERIFY] routine real cycles = %d\n", real_cycles);
            for (int i = 0; i < 16; i++) {
                uint32_t computed = m68k_read_disassembler_32(
                    s_cache_verify_a5 + 0x40 + (unsigned)(i * 4));
                uint32_t expected = s_cache_verify_pre[i] + s_cache_delta[i];
                if (computed != expected) {
                    if (s_cache_verify_mismatches < 20)
                        fprintf(stderr, "[VERIFY MISMATCH] A5=%06x +$%02x: "
                                "delta_applied=%08x real=%08x\n",
                                s_cache_verify_a5, 0x40 + i * 4,
                                expected, computed);
                    s_cache_verify_mismatches++;
                }
            }
        }
#endif
    }
#endif /* SCD_CACHE */

#ifndef SCD_CACHE_ONLY
    if (s_frozen) return;

    if (s_clear_pending) {
        memset(g_op_hist_main, 0, sizeof(g_op_hist_main));
        memset(g_op_hist_sub,  0, sizeof(g_op_hist_sub));
        memset(g_pc_hist_main, 0, sizeof(g_pc_hist_main));
        memset(g_pc_hist_sub,  0, sizeof(g_pc_hist_sub));
        s_clear_pending = 0;
        s_armed = 1;
        fprintf(stderr, "[hist] armed (cleared at hook tick)\n");
    }
    if (!s_armed) return;

    /* opcode word sits at PC; the dispatch loop has not set REG_IR yet at the
     * hook site, so fetch it ourselves. One extra read per insn — cheap on host. */
    unsigned int op = m68k_read_disassembler_16(pc);

    if (g_scd_hist_issub) {
        g_op_hist_sub[op & 0xFFFFu]++;
        g_pc_hist_sub [pc & HIST_PC_MASK]++;
    } else {
        g_op_hist_main[op & 0xFFFFu]++;
        g_pc_hist_main[pc & HIST_PC_MASK]++;
    }

    /* $7c80 trace: capture rotation-calc inputs for caching analysis */
    if (g_scd_hist_issub && pc == 0x7c80 && s_rot_trace_n < ROT_TRACE_MAX) {
        unsigned int a5 = m68k_get_reg(M68K_REG_A5);
        s_rot_trace[s_rot_trace_n].frame = g_scd_frame;
        s_rot_trace[s_rot_trace_n].a5 = a5;
        for (int i = 0; i < 16; i++)
            s_rot_trace[s_rot_trace_n].data[i] =
                m68k_read_disassembler_32(a5 + (unsigned)(i * 4));
        s_rot_trace_n++;
    }
#endif /* !SCD_CACHE_ONLY */
}

cpu_hook_fn cpu_hook = scd_hist_hook;

void scd_hist_clear(void)
{
    s_clear_pending = 1;
}

/* ---- dump helpers ---- */

#ifndef SCD_CACHE_ONLY

struct top_entry { uint32_t idx; uint32_t cnt; };

/* naive top-N by repeated max scan; arrays are small (16 or 2 M) and we only
 * run this once at exit, so simplicity wins over a full sort. For the PC
 * histogram we cap the scan to HIST_PC_SIZE entries. */
static void top_n(const uint32_t *arr, uint32_t arr_size, struct top_entry *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { out[i].idx = 0; out[i].cnt = 0; }
    if (arr_size == 0) return;
    for (uint32_t i = 0; i < arr_size; i++) {
        uint32_t v = arr[i];
        if (v == 0) continue;
        /* insert into the n-slot table if it beats the current minimum */
        uint32_t min_pos = 0;
        for (uint32_t j = 1; j < n; j++)
            if (out[j].cnt < out[min_pos].cnt) min_pos = j;
        if (v > out[min_pos].cnt) {
            /* de-dup: idx is unique by construction (one slot per i) */
            out[min_pos].idx = i;
            out[min_pos].cnt = v;
        }
    }
    /* descending bubble over the tiny top-N so output reads top-down */
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++)
            if (out[j].cnt > out[i].cnt) {
                struct top_entry t = out[i]; out[i] = out[j]; out[j] = t;
            }
}

static uint64_t total_count(const uint32_t *arr, uint32_t arr_size)
{
    uint64_t s = 0;
    for (uint32_t i = 0; i < arr_size; i++) s += arr[i];
    return s;
}

static void dump_one(const char *name,
                     const uint32_t *oph, const uint32_t *pch)
{
    struct top_entry top_op[TOP_N], top_pc[TOP_N_PC];
    top_n(oph, HIST_OP_SIZE, top_op, TOP_N);
    top_n(pch, HIST_PC_SIZE, top_pc, TOP_N_PC);
    uint64_t op_total = total_count(oph, HIST_OP_SIZE);

    printf("\n========== %s 68K  insn-sampled total=%llu ==========\n",
           name, (unsigned long long)op_total);

    printf("  -- top-%u OPCODES --\n", TOP_N);
    for (uint32_t i = 0; i < TOP_N; i++) {
        if (top_op[i].cnt == 0) break;
        printf("    op %04x  %9u  %5.2f%%\n",
               top_op[i].idx, top_op[i].cnt,
               op_total ? 100.0 * top_op[i].cnt / (double)op_total : 0.0);
    }

    printf("  -- top-%u PCs (low %u bits) --\n", TOP_N_PC, HIST_PC_BITS);
    for (uint32_t i = 0; i < TOP_N_PC; i++) {
        if (top_pc[i].cnt == 0) break;
        printf("    pc %06x  %9u  %5.2f%%\n",
               top_pc[i].idx, top_pc[i].cnt,
               op_total ? 100.0 * top_pc[i].cnt / (double)op_total : 0.0);
    }
}

#endif /* !SCD_CACHE_ONLY */

void scd_hist_dump(void)
{
#ifndef SCD_CACHE_ONLY
    s_frozen = 1;
    /* disarm the hook so dump itself doesn't add noise if we keep running */
    cpu_hook = NULL;
    dump_one("MAIN", g_op_hist_main, g_pc_hist_main);
    dump_one("SUB ", g_op_hist_sub,  g_pc_hist_sub);

    /* $7c80 trace dump — show stamp descriptor inputs per call */
    if (s_rot_trace_n > 0) {
        printf("\n========== $7c80 ROTATION-CALC INPUT TRACE (%d entries) ==========\n", s_rot_trace_n);
        for (int i = 0; i < s_rot_trace_n; i++) {
            printf("  [%3d] f=%3d a5=%06x:", i, s_rot_trace[i].frame, s_rot_trace[i].a5);
            for (int j = 0; j < 16; j++) printf(" %08x", s_rot_trace[i].data[j]);
            printf("\n");
        }
        /* quick frame-to-frame diff: group by frame, XOR first entry of each frame */
        printf("\n  -- frame-to-frame diff (first entry per frame) --\n");
        int prev_frame = -1; unsigned int prev_data[16] = {0};
        for (int i = 0; i < s_rot_trace_n; i++) {
            if (s_rot_trace[i].frame != prev_frame) {
                if (prev_frame >= 0) {
                    int diffs = 0;
                    for (int j = 0; j < 16; j++)
                        if (s_rot_trace[i].data[j] != prev_data[j]) diffs++;
                    printf("    f%d→f%d: %d/16 longwords changed\n",
                           prev_frame, s_rot_trace[i].frame, diffs);
                }
                prev_frame = s_rot_trace[i].frame;
                memcpy(prev_data, s_rot_trace[i].data, sizeof(prev_data));
            }
        }
    }

    printf("\n[hist] dump complete.\n");
#endif /* !SCD_CACHE_ONLY */

#ifdef SCD_CACHE
    printf("\n[$7c80 cache] hits=%u misses=%u hit_rate=%.1f%%\n",
           s_cache_hits, s_cache_misses,
           (s_cache_hits + s_cache_misses) ?
               100.0 * s_cache_hits / (double)(s_cache_hits + s_cache_misses) : 0.0);
#ifdef SCD_CACHE_VERIFY
    printf("[$7c80 cache VERIFY] checks=%u mismatches=%u"
           " avg_cycles=%.1f (samples=%d)\n",
           s_cache_verify_checks, s_cache_verify_mismatches,
           s_cache_verify_cycle_samples ?
               (double)s_cache_verify_cycle_sum / s_cache_verify_cycle_samples : 0.0,
           s_cache_verify_cycle_samples);
#endif
#endif
    printf("[sub insn] total=%llu\n", (unsigned long long)g_sub_insn_count);
}
