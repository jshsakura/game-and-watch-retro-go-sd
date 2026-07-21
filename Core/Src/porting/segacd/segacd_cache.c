/* segacd_cache.c — $7c80 rotation-coefficient delta cache (firmware).
 *
 * See segacd_cache.h for the architecture.  This file is the self-contained
 * firmware version of the harness cache in tools/segacd_harness/scd_hist.c.
 * No histogram, no trace, no stdio — just the cache.
 *
 * Build requirements: -DHOOK_CPU -DSCD_CACHE  (cpuhook.h must be on the
 * include path, typically Core/Inc/).
 */
#include <stddef.h>
#include <stdio.h>       /* FILE — used by m68k.h savestate prototypes */
#include "m68k.h"
#include "cpuhook.h"
#include "segacd_cache.h"

/* ---- cache state ---- */
static uint32_t s_key[16];     /* input params  A5+$00-$3F (64 bytes) */
static uint32_t s_delta[16];   /* cached deltas A5+$40-$7F: after - before */
static uint32_t s_before[16];  /* A5+$40-$7F snapshot before routine runs */
static int      s_valid   = 0; /* cache filled? */
static int      s_pending = 0; /* MISS in progress, waiting for RTS */
static uint32_t s_a5      = 0; /* A5 captured at MISS entry */
static uint32_t s_hits    = 0;
static uint32_t s_misses  = 0;

/* set by segacd_engine.c when the sub-68K context is active */
extern int g_scd_hist_issub;

/* ---- low-level helpers ---- */

/* Write 32 bits to 68K memory via the memory_map (callback or base ptr).
 * Mirrors m68ki_write_32 but bypasses the dispatch overhead. */
static void write32_direct(uint32_t addr, uint32_t val)
{
    cpu_memory_map *m = &m68k.memory_map[(addr >> 16) & 0xFF];
    if (m->write16) {
        (*m->write16)(addr & 0xFFFFFF,        (val >> 16) & 0xFFFF);
        (*m->write16)((addr + 2) & 0xFFFFFF,   val        & 0xFFFF);
    } else if (m->base) {
        unsigned char *p = m->base + (addr & 0xFFFF);
        p[0] = (val >> 24) & 0xFF;
        p[1] = (val >> 16) & 0xFF;
        p[2] = (val >>  8) & 0xFF;
        p[3] =  val        & 0xFF;
    }
}

/* ---- hook implementation ---- */

static void scd_cache_hook(int type, int size, unsigned int addr, unsigned int val)
{
    (void)size; (void)val;
    if (type != HOOK_M68K_E) return;
    if (!g_scd_hist_issub)   return;

    unsigned int pc = addr;

    if (pc == 0x7c80) {
        /* ---- entry to rotation-calc routine ---- */
        unsigned int a5 = m68k_get_reg(M68K_REG_A5);
        uint32_t inputs[16];
        int match = s_valid;

        for (int i = 0; i < 16; i++) {
            inputs[i] = m68k_read_disassembler_32(a5 + (unsigned)(i * 4));
            if (match && inputs[i] != s_key[i]) match = 0;
        }

        if (match) {
            /* HIT: apply cached deltas to current accumulators */
            for (int i = 0; i < 16; i++) {
                uint32_t cur = m68k_read_disassembler_32(
                    a5 + 0x40u + (unsigned)(i * 4));
                write32_direct(a5 + 0x40u + (unsigned)(i * 4),
                               cur + s_delta[i]);
            }
            /* Skip routine: pop return address, advance cycles.
             * $7c80 preserves D2-D7/A2-A6 (callee-saved); D0-D1/A0-A1
             * are scratch and reinitialised by $7cd0 from the A5 struct. */
            unsigned int sp  = m68k_get_reg(M68K_REG_A7);
            unsigned int ret = m68k_read_disassembler_32(sp);
            m68k_set_reg(M68K_REG_A7, sp + 4);
            m68k_set_reg(M68K_REG_PC, ret);
            m68k.cycles += 800;   /* approximate routine cost */
            s_hits++;
            return;
        }

        /* MISS: save key + before-snapshot, let routine run */
        for (int i = 0; i < 16; i++) {
            s_key[i]    = inputs[i];
            s_before[i] = m68k_read_disassembler_32(
                a5 + 0x40u + (unsigned)(i * 4));
        }
        s_a5      = a5;
        s_pending = 1;
        s_misses++;

    } else if (s_pending && pc == 0x7cce) {
        /* ---- RTS of $7c80 — compute deltas ---- */
        for (int i = 0; i < 16; i++) {
            uint32_t after = m68k_read_disassembler_32(
                s_a5 + 0x40u + (unsigned)(i * 4));
            s_delta[i] = after - s_before[i];
        }
        s_valid   = 1;
        s_pending = 0;
    }
}

/* ---- public API ---- */

void scd_cache_hook_enable(int enable)
{
    cpu_hook = enable ? scd_cache_hook : NULL;
}

void scd_cache_stats(uint32_t *hits, uint32_t *misses)
{
    *hits   = s_hits;
    *misses = s_misses;
}
