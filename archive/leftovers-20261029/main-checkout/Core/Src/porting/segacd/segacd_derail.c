/* segacd_derail.c — sub-68K derail tracer (bring-up only, never ships).
 *
 * The AUGCD sub-BIOS derails around frame ~0x59: PC lands in its own CDD
 * record buffer at $97E8 (data, not code) and executes garbage until halt.
 * Static disassembly proved the derail cannot come from the $609a(pc,d0.w)
 * record-dispatch jsr (valid d0 values are small even numbers; $97E8 needs
 * d0=$374C), so some other indirect transfer (jumptable at $5f00-$5f48,
 * CDD sender $2e1c, INT4 handlers $676+) corrupts control flow.
 *
 * This tracer hooks the HOOK_CPU per-instruction point (sub context only,
 * via g_scd_hist_issub) and keeps a ring of the last control-flow events in
 * the watch zones, plus a frozen full-register shot at the FIRST entry into
 * the data zone $8800-$FFFF. Read both live via OpenOCD mdw:
 *
 *   scd_derail_ring[64]   16B entries {tag<<16|pc, d0, a6, prev_pc}
 *   scd_derail_shot       frozen {pc, prev, d0-d7, a0-a7, sp} at first derail
 *   scd_derail_count      watch events pushed (u32)
 *   scd_derail_hits       instructions executed in the data zone (u32)
 *
 * Build: SEGACD_DERAIL=1 (adds -DSEGACD_DERAIL -DHOOK_CPU to SEGACD_C_DEFS,
 * so m68kcpu.c gains the dispatch-loop hook call). Nothing here compiles or
 * links otherwise. All BSS lands in .overlay_segacd_bss via the segacd
 * object wildcard (main BSS is full — do not move these to default bss).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>       /* FILE — m68k.h savestate prototypes */
#include "m68k.h"
#include "cpuhook.h"
#include "segacd_derail.h"

#ifdef SEGACD_DERAIL

/* m68kcpu.c only REFERENCES cpu_hook; the device harness used to provide
 * the definition. This build owns it (segacd TUs only — the MD copy of
 * m68kcpu.c compiles without -DHOOK_CPU, so no duplicate symbol). */
cpu_hook_fn cpu_hook;

/* set by segacd_engine.c around the sub-context swap */
extern int g_scd_hist_issub;

/* defined here: engine.c's HOOK_CPU attribution block references it and the
 * old definition lived in harness-only code that is not linked on device. */
int g_scd_hist_issub = 0;

#define DERAIL_RING_N 32

struct derail_ent {
    uint32_t tag_pc;   /* 'W'<<24 | pc (watch zone)  /  'D'<<24 | pc (data) */
    uint32_t d0;
    uint32_t a6;
    uint32_t prev;     /* sub pc executed immediately before this one */
};

struct derail_shot {
    uint32_t pc, prev;
    uint32_t d[8];
    uint32_t a[8];     /* a[7] = A7/SP */
};

struct derail_ent  scd_derail_ring[DERAIL_RING_N];
struct derail_shot scd_derail_shot;
uint32_t scd_derail_count;   /* watch events pushed */
uint32_t scd_derail_hits;    /* instructions executed in data zone */
uint32_t scd_derail_seen;    /* first derail latched (freeze shot) */

static uint32_t s_prev;      /* previous sub PC (cheap per-insn tracking) */
static uint32_t s_idx;

/* watch zones (sub PRG page 0, logical addresses) */
#define IN(p, lo, len) ((unsigned)((p) - (lo)) < (len))

static void derail_push(uint32_t tag, uint32_t pc, uint32_t d0, uint32_t a6)
{
    struct derail_ent *e = &scd_derail_ring[s_idx];
    e->tag_pc = (tag << 24) | (pc & 0xffffff);
    e->d0     = d0;
    e->a6     = a6;
    e->prev   = s_prev;
    s_idx     = (s_idx + 1) & (DERAIL_RING_N - 1);
    scd_derail_count++;
}

static void segacd_derail_hook(int type, int size, unsigned int addr, unsigned int val)
{
    uint32_t pc;
    (void)size; (void)val;
    if (type != HOOK_M68K_E) return;
    if (!g_scd_hist_issub)   return;

    pc = addr;

    /* data-zone derail: freeze one full shot on first entry */
    if (IN(pc, 0x8800, 0x7800)) {
        scd_derail_hits++;
        if (!scd_derail_seen) {
            int i;
            scd_derail_seen = 1;
            scd_derail_shot.pc  = pc;
            scd_derail_shot.prev = s_prev;
            for (i = 0; i < 8; i++) scd_derail_shot.d[i] = m68k_get_reg(M68K_REG_D0 + i);
            for (i = 0; i < 8; i++) scd_derail_shot.a[i] = m68k_get_reg(M68K_REG_A0 + i);
            derail_push('D', pc, scd_derail_shot.d[0], scd_derail_shot.a[6]);
        }
        s_prev = pc;
        return;
    }

    /* watch zones: INT2 jump table+dispatch, record engine, INT4 chain,
     * CDD sender — the plausible sources of the wild transfer */
    if (IN(pc, 0x5f00, 0x148) || IN(pc, 0x6060, 0x140) ||
        IN(pc, 0x0676, 0x0b0) || IN(pc, 0x2e00, 0x060) ||
        IN(pc, 0x6300, 0x100)) {
        derail_push('W', pc,
                    m68k_get_reg(M68K_REG_D0),
                    m68k_get_reg(M68K_REG_A6));
    }

    s_prev = pc;
}

void segacd_derail_enable(int enable)
{
    cpu_hook = enable ? segacd_derail_hook : NULL;
}

#else /* !SEGACD_DERAIL */

void segacd_derail_enable(int enable) { (void)enable; }

#endif
