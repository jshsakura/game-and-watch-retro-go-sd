/* Sega 32X (picodrive, GNW trimmed set) on QEMU's Cortex-M7 (mps2-an500):
 * executed-instruction count per frame on a real ARMv7-M *Thumb* stream.
 *
 * This rig compiles the SAME trimmed source set as the device overlay
 * (-DGNW_32X_CORE -DEMU_G68K -DTABLES_FULL -D_USE_CZ80: gwenesis 68K + cz80 +
 * SH-2 interpreter) and mirrors Core/Src/porting/md32x/main_md32x.c's init
 * order EXACTLY — a partial init hung PicoFrame here before (SH-2 clock
 * multiplier unset), and a host build cannot see Thumb-only faults at all
 * (the map function-pointer bit0 class this rig exists to gate).
 *
 * The ROM blob is PRE-BYTESWAPPED by run_32x.sh (16-bit byteswap), mirroring
 * the device flash cache (byte_swap=true): the GNW zero-copy path in
 * pico/cart.c binds Pico.rom to the passed buffer and skips Byteswap().
 *
 * SUCCESS = PicoFrame returns continuously, framebuffer non-blank and
 * changing across frames, avg host insn/frame reported.
 *
 * Optional modes (pass via EXTRA_DEF to run_32x.sh):
 *   -DRIG_SKIP3      device-shaped frameskip: PicoIn.skipFrame=1 on 2 of every
 *                    3 frames (common_emu_frame_loop drops frames under load).
 *                    Reports drawn-frame vs skipped-frame insn averages —
 *                    the frameskip headroom the device leans on.
 *   -DRIG_PAD_SCRIPT scripted PicoIn.pad[0] (START around f120, then
 *                    directional+button mash) — proves the pad reaches the
 *                    68K IO when the fb trace diverges from a no-input run.
 *   -DRIG_TRACE_CKS  print the fb checksum EVERY frame (diff two runs to
 *                    find the first divergence frame).
 *   -DRIG_TRACE_PC   print the 68K PC every frame (where does a dead boot
 *                    park?). g68k backend only.
 *   -DRIG_STATE_TEST save after 120 warm-up frames, run 30 frames, restore,
 *                    replay those frames, and require identical framebuffer
 *                    checksums. Exercises the device's PicoStateFP path.
 *   -DRIG_STATE_LOAD resume THE DEVICE'S OWN savestate (RIG_32X_STATE=<file>
 *                    to run_32x.sh links the .sav in as a blob). Every 32X
 *                    profile taken here before this was the front-end wait
 *                    loop -- 188 unique guest PCs in every reachable state,
 *                    attract and pad-script alike -- so nothing offline had
 *                    ever profiled Doom rendering. This resumes the exact
 *                    scene the device measures, forever, offline. See
 *                    docs/32X_HANDOFF_0818.md.
 *   -DRIG_PHASE_PROF per-phase cost table (PHASE_PROF=1 to run_32x.sh): rides
 *                    picodrive's pprof probes with the icount timer as clock,
 *                    so every bucket is an executed-instruction count. Buckets
 *                    are disjoint (nested phases pause the enclosing one via
 *                    pprof_end_sub); "other" = PicoFrame minus the sum =
 *                    scheduler, events, timers, memory glue.
 * Soak drift (first-500 vs last-500 post-warmup insn/frame) is reported
 * automatically when RIG_FRAMES is large enough for disjoint windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "pico/pico_types.h"
#include "pico/pico.h"
#include "cpu/sh2/sh2.h"
extern SH2 *sh2s;  /* pico_int.h:273 equivalent without the PicoDrawSetOutputSMS collision */  /* SH2, sh2s (pointer since the DTCM move); NOT pico_int.h -- it declares PicoDrawSetOutputSMS(pdso_t) which collides with the rig stub at :246 (learned 2026-08-26, rig build broke in 6378f72e) */
#if defined(RIG_STATE_TEST) || defined(RIG_STATE_LOAD)
#include "pico/state.h"
#endif
#if defined(RIG_TRACE_PC) || defined(RIG_SH2_WATCH)
#include <cpu/gwenesis68k/g68k.h>   /* the g68k global context (m68k.pc) */

/* SRAM replay (compile-time, no semihosting file I/O — SYS_OPEN returns -1 in
 * this runtime while SYS_WRITE0/printf work; proven by two failed probe runs):
 * host generates the header via `cp <file> rig_preload.bin; xxd -i` and passes
 * -DRIG_SRAM_PRELOAD_HEADER="\"/abs/path.h\"". */
#ifdef RIG_SRAM_PRELOAD_HEADER
#include RIG_SRAM_PRELOAD_HEADER
#endif
#endif
#ifdef RIG_SDRAM_SCAN
#include <cpu/gwenesis68k/g68k.h>
struct Pico32xMem;                  /* opaque: sdram sits at offset 0 */
extern struct Pico32xMem *Pico32xMem;
static int s_zscan_done;
#endif
#ifdef RIG_STRPAGE
/* GLM: reroute 68K page 0x8a (ROM offsets 0x20000-0x2ffff — the window
 * holding "Z_Malloc: failed on") through handlers that exactly
 * replicate the direct pre-byteswapped ROM mapping, logging any read of the
 * string window. The reader's m68k.pc is the error printer; its caller is
 * the Z_Malloc failure path. Installed at f==8, AFTER the game's ADEN write
 * ran Pico32xStartup — PicoMemSetup32x would overwrite an earlier install.
 * Both r8+r16 are hooked so byte-wise printers are caught; g68k_bus.c keeps
 * the page's fetch base and routes fetches through the dispatchers, which
 * land on these handlers — values stay exact. */
#include <cpu/gwenesis68k/g68k.h>   /* m68k.pc */
extern uptr m68k_read8_map[];
extern uptr m68k_read16_map[];
void cpu68k_map_set(uptr *map, u32 start_addr, u32 end_addr,
    const void *func_or_mh, int is_func);
/* The string's ROM offset moves between D32XR builds -- 0xdbdc in the 4 MiB
 * bench cut, 0x4c9c in the official 5 MiB release -- and a window pinned to one
 * of them watches nothing in the other while still reporting "0 hits", which
 * reads exactly like "the 68K never touched it". Override per ROM:
 *   EXTRA_DEF="-DRIG_STRPAGE -DRIG_STRPAGE_ADDR=0x8a4c9c"
 * Find it with: python3 -c "print(hex(open(ROM,'rb').read().find(b'Z_Malloc')))"
 * and add 0x8a0000 - 0x20000. */
#ifndef RIG_STRPAGE_ADDR
#define RIG_STRPAGE_ADDR 0x8adbdc
#endif
#define RIG_STRP_LO ((RIG_STRPAGE_ADDR) & ~0xffu)
#define RIG_STRP_HI (RIG_STRP_LO + 0x100u)

static int s_strp_hits;
static unsigned char *s_rom_base;
extern int rig_frame_no;  /* declared later in this file (L137) */
extern const unsigned char _binary_rom_32x_start[];  /* linker symbol, also later (L140) */
static u32 rig_strp_r8(u32 a)
{
  if (a >= RIG_STRP_LO && a < RIG_STRP_HI && s_strp_hits < 32) {
    s_strp_hits++;
    lprintf("[zrd] f=%d r8 pc=%08x a=%08x\n", rig_frame_no,
            (unsigned)(m68k.pc & 0xffffff), (unsigned)a);
  }
  return s_rom_base[(a - 0x880000) ^ 1];  /* pre-byteswapped rom */
}
static u32 rig_strp_r16(u32 a)
{
  if (a >= RIG_STRP_LO && a < RIG_STRP_HI && s_strp_hits < 32) {
    s_strp_hits++;
    lprintf("[zrd] f=%d r16 pc=%08x a=%08x\n", rig_frame_no,
            (unsigned)(m68k.pc & 0xffffff), (unsigned)a);
  }
  return *(u16 *)(s_rom_base + (a - 0x880000));
}

static int s_strp_installed;
static void rig_strp_install(void)
{
  /* zero-copy GNW path: cart.c binds Pico.rom to the blob we passed to
   * PicoLoadMedia (see header comment at top of file) — same bytes. */
  s_rom_base = (unsigned char *)_binary_rom_32x_start;
  cpu68k_map_set(m68k_read8_map, 0x8a0000, 0x8affff, rig_strp_r8, 1);
  cpu68k_map_set(m68k_read16_map, 0x8a0000, 0x8affff, rig_strp_r16, 1);
  printf("[zrd] string page 0x8a rerouted at f=%d\n", rig_frame_no);
}
#endif

#ifdef RIG_DEATH_STACK
/* GLM: Z_Malloc caller hunt. At frame RIG_DEATH_STACK dump both SH-2s'
 * raw register windows (r[16] @0x00, pc @0x40, ppc @0x44, pr @0x48, sr @0x4c
 * -- offsets verified with gdb ptype /o SH2 on the device ELF) and the
 * master's stack above r15, then for every SDRAM-range (0x0600xxxx) value
 * found in that stack dump 48 halfwords of code around it, so one run
 * yields the whole frozen call chain (the guest parks inside I_Error, the
 * stack keeps the failing path). ROM-range (0x0202xxxx) return addrs are
 * resolved offline from the ROM file (offset = guest & 0x3fffff). */
struct Pico32xMem;                  /* opaque: sdram sits at offset 0 */
extern struct Pico32xMem *Pico32xMem;
static int s_ds_done;
#endif

#ifndef RIG_FRAMES
#define RIG_FRAMES 600
#endif
/* Frames before the measurement window opens (and where phase_snapshot fires).
 * Overridable because RIG_STATE_LOAD moves where "warm" begins: with a resumed
 * savestate the boot frames before the load are a different program, and
 * averaging them in reports neither. Use -DRIG_WARMUP=<load frame + 1>. */
#ifndef RIG_WARMUP
#define RIG_WARMUP 20
#endif
#ifndef RIG_MIX_FROM
/* frame at which RIG_MEM_MIX zeroes its census -- the pad script's gameplay
 * entry (see pad_script); override on the command line if that moves.
 *
 * WARNING, measured 0726: a census run that has to REACH gameplay is not
 * practical. Getting to frame 400 costs ~13 minutes; each gameplay frame after
 * it costs 4+ minutes under icount with the counters in, so three of them
 * outran run_32x.sh's own RIG_TIMEOUT twice and the report -- which only
 * prints after the loop -- never printed at all. If you revive this, print the
 * census EVERY frame from RIG_MIX_FROM so a killed run still yields data.
 * The better instrument now lives on the device (md32x_profile.c's
 * data-access probe): QEMU models no cache, so it can count accesses but can
 * never price them, which was the actual question. */
#ifndef RIG_MIX_FROM
#define RIG_MIX_FROM 400
#endif
#endif

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* SH-2 executed-instruction counter (fork: cpu/sh2/mame/sh2pico.c under
 * -DRIG_SH2_COUNT. Proves the SH-2s actually run — 0 here was the old hang's
 * signature (68000 spinning on a dead SH-2). */
extern unsigned long long g_sh2_insns;

/* GLM debug: frame counter shared with the instrumented picodrive copy (pd_dbg) */
int rig_frame_no;

/* ROM blob (objcopy .rom section) — already 16-bit byteswapped */
extern const unsigned char _binary_rom_32x_start[];
#ifdef RIG_32X_BIOS
/* Real 32X BIOS, three blobs, linked in the same way as the ROM.
 *
 * Without them picodrive synthesises the BIOS's effects (memory.c's
 * msh2_code[]): a delay loop, an M_OK write and a jump to the cartridge's
 * master entry. That is enough for the retail games; it was NOT enough for
 * D32XR, whose SH-2s finish their own init and then sit in poll loops
 * (0x02013ca2 master, 0x0600090a slave) waiting for something the stub never
 * does. This arm exists to tell those two cases apart. */
extern const unsigned char _binary_bios_m68k_start[], _binary_bios_m68k_end[];
extern const unsigned char _binary_bios_msh2_start[], _binary_bios_msh2_end[];
extern const unsigned char _binary_bios_ssh2_start[], _binary_bios_ssh2_end[];
#endif
extern const unsigned char _binary_rom_32x_end[];

/* ==== device-shim set: mirrors main_md32x.c one for one ==================== */

/* picodrive platform hooks (DRC-less interpreter: only small allocs) */
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed) {
    (void)addr; (void)need_exec; (void)is_fixed;
    return malloc(size);
}
void *plat_mremap(void *ptr, size_t oldsize, size_t newsize) {
    (void)oldsize; return realloc(ptr, newsize);
}
void plat_munmap(void *ptr, size_t size) { (void)size; free(ptr); }
int  plat_mem_set_exec(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }

void lprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
/* crc32: run_32x.sh links zlib/crc32.c (real values — carthw detection),
 * like the device forwards to the firmware's real crc32_le. */

/* zip/gzip loading is a desktop path; honest fail-stubs, mirroring the device */
void *openzip(const char *path) { (void)path; return NULL; }
void  closezip(void *zip) { (void)zip; }
int   readzip(void *zip) { (void)zip; return -1; }
int   seekcompresszip(void *zip, void *ent) { (void)zip; (void)ent; return -1; }
int   inflateInit2_(void *strm, int wbits, const char *ver, int ssize)
      { (void)strm; (void)wbits; (void)ver; (void)ssize; return -2; }
int   inflate(void *strm, int flush) { (void)strm; (void)flush; return -2; }
int   inflateReset(void *strm) { (void)strm; return -2; }
int   inflateEnd(void *strm) { (void)strm; return 0; }

/* SMS renderer TU is excluded; unreachable for 32X. NOTE: PicoDraw2SetOutBuf
 * must NOT be a no-op stub — the 32X compositor needs the Draw2FB frame it
 * binds (see rig_32x_draw2fb.c; a NULL Draw2FB = wild pmd reads). */
void PicoDrawSetOutputSMS(int which) { (void)which; }

/* 68K 64K bank image — device: ahb_calloc(1, 0x10000); rig: static (zeroed) */
static unsigned char s_m68k_bank[0x10000];
unsigned char *gnw_m68k_bank_alloc(void) { return s_m68k_bank; }

/* ==== phase profiler ======================================================= */
#ifdef RIG_PHASE_PROF
/* Counter storage for picodrive's pprof probes (pico_int.h defines PPROF when
 * RIG_PHASE_PROF is set; platform/linux/pprof.h routes pprof_get_one() to
 * rig_timer_now(), so a bucket delta IS an executed-instruction count after
 * the insn/tick calibration). */
#include "platform/linux/pprof.h"

static struct pp_counters s_pp_counters;
struct pp_counters *pp_counters = &s_pp_counters;
static int s_pp_refcounts[pp_total_points];
int *refcounts = s_pp_refcounts;

static pp_type s_pp_base[pp_total_points];

static const struct { unsigned char pt; const char *name; } k_phase_rows[] = {
    { pp_m68k,    "m68k (interp+bus)" },
    { pp_msh2,    "msh2 (interp+bus)" },
    { pp_ssh2,    "ssh2 (interp+bus)" },
    { pp_z80,     "z80  (interp+bus)" },
    { pp_fm,      "fm   (ym2612)"     },
    { pp_pwm,     "pwm  (chip)"       },
    { pp_sound,   "snd  (psg+dac+mix)"},
    { pp_draw,    "draw (MD VDP line)"},
    { pp_draw32x, "32x  (compositor)" },
};
#define N_PHASE_ROWS ((int)(sizeof(k_phase_rows) / sizeof(k_phase_rows[0])))

static void phase_snapshot(void) {
    memcpy(s_pp_base, s_pp_counters.counter, sizeof(s_pp_base));
}

/* post-warmup insn/frame for one bucket */
static uint64_t phase_insn(int pt, int frames, uint32_t ipt_x1000) {
    pp_type ticks = s_pp_counters.counter[pt] - s_pp_base[pt];
    return (uint64_t)ticks * ipt_x1000 / 1000 / (uint32_t)(frames > 0 ? frames : 1);
}

static void phase_print_row(const char *name, uint64_t v, uint64_t total) {
    uint64_t pm = total ? v * 1000 / total : 0;   /* permille */
    printf("[32x-phase]   %-20s %9llu  %3llu.%llu%%\n", name,
           (unsigned long long)v, (unsigned long long)(pm / 10),
           (unsigned long long)(pm % 10));
}

static void phase_report(int frames, uint32_t ipt_x1000, uint64_t sh2_guest_avg) {
    uint64_t total = phase_insn(pp_frame, frames, ipt_x1000);
    uint64_t sum = 0;
    printf("[32x-phase] host insn/frame by phase (%d frames post-warmup):\n", frames);
    for (int i = 0; i < N_PHASE_ROWS; i++) {
        uint64_t v = phase_insn(k_phase_rows[i].pt, frames, ipt_x1000);
        sum += v;
        phase_print_row(k_phase_rows[i].name, v, total);
    }
    phase_print_row("other(sched/ev/mem)", total > sum ? total - sum : 0, total);
    phase_print_row("PicoFrame TOTAL", total, total);

    /* SH-2 interpreter host-per-guest ratio (x1000) */
    {
        uint64_t sh2_host = phase_insn(pp_msh2, frames, ipt_x1000)
                          + phase_insn(pp_ssh2, frames, ipt_x1000);
        uint64_t r_x1000 = sh2_guest_avg ? sh2_host * 1000 / sh2_guest_avg : 0;
        printf("[32x-phase] sh2 host/guest: %llu host / %llu guest insn = %llu.%03llu\n",
               (unsigned long long)sh2_host, (unsigned long long)sh2_guest_avg,
               (unsigned long long)(r_x1000 / 1000), (unsigned long long)(r_x1000 % 1000));
    }

    /* refcount leak = a pprof scope escaped (early return) — data suspect */
    for (int i = 0; i < pp_total_points; i++)
        if (s_pp_refcounts[i] != 0)
            printf("[32x-phase] WARN refcount leak: point %d = %d\n", i, s_pp_refcounts[i]);
}
#else
static void phase_snapshot(void) {}
static void phase_report(int frames, uint32_t ipt_x1000, uint64_t sh2_guest_avg) {
    (void)frames; (void)ipt_x1000; (void)sh2_guest_avg;
}
#endif

/* ==== frame histogram ====================================================== */
#ifdef RIG_FRAME_HIST
/* Per-frame host-instruction cost distribution. The single average hides
 * bimodal drawn/skip cost and demo-scene drift; the histogram exposes the
 * spread (p50/p90/p95/p99) so two runs can be compared by distribution
 * shape, not just mean. Byte-identical to an off build: collects data only,
 * never alters PicoFrame's control flow. */
static uint32_t s_fh_drawn[RIG_FRAMES];
static uint32_t s_fh_skip[RIG_FRAMES];
static uint32_t s_fh_n_drawn, s_fh_n_skip;

static int fh_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void fh_report(const char *tag, const uint32_t *arr, uint32_t n) {
    if (n == 0) { printf("[32x-hist] %s: n=0\n", tag); return; }
    uint32_t *tmp = (uint32_t *)malloc(sizeof(uint32_t) * n);
    if (!tmp) { printf("[32x-hist] %s: alloc fail n=%u\n", tag, n); return; }
    memcpy(tmp, arr, sizeof(uint32_t) * n);
    qsort(tmp, n, sizeof(uint32_t), fh_cmp);
    uint32_t mn = tmp[0], mx = tmp[n - 1];
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += tmp[i];
    printf("[32x-hist] %s: n=%u min=%u max=%u avg=%llu p50=%u p90=%u p95=%u p99=%u\n",
           tag, n, mn, mx, (unsigned long long)(sum / n),
           tmp[(uint32_t)((uint64_t)n * 50 / 100)],
           tmp[(uint32_t)((uint64_t)n * 90 / 100)],
           tmp[(uint32_t)((uint64_t)n * 95 / 100)],
           tmp[(uint32_t)((uint64_t)n * 99 / 100)]);
    if (mx > mn) {
        uint32_t bins[20] = {0};
        uint64_t span = (uint64_t)mx - mn;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t b = (uint32_t)(((uint64_t)(tmp[i] - mn) * 20) / span);
            if (b >= 20) b = 19;
            bins[b]++;
        }
        printf("[32x-hist] %s 20-bin distribution:\n", tag);
        for (int b = 0; b < 20; b++) {
            uint64_t lo = mn + span * (unsigned)b / 20;
            uint64_t hi = mn + span * (unsigned)(b + 1) / 20;
            int bar = (int)((uint64_t)bins[b] * 40 / n);
            printf("[32x-hist]   [%9llu-%9llu) %6u  %.*s\n",
                   (unsigned long long)lo, (unsigned long long)hi, bins[b],
                   bar, "****************************************");
        }
    }
    free(tmp);
}
#endif /* RIG_FRAME_HIST */

/* ==== SH-2 guest-PC histogram ============================================== */
#ifdef RIG_SH2_PC_HIST
/* Reads the sparse tables filled by sh2pico.c's RIG_PC_HIST_TICK (one per core:
 * master/slave). Top-N PCs by total guest-instruction count, with direct vs
 * delay-slot breakdown, cumulative %, and SH-2 disassembly of each hot PC.
 * Run fastloop-OFF first to see the loops fastloop kills, then ON to see the
 * residual hot set. */
#include "cpu/sh2/mame/sh2dasm.h"
/* Must match sh2pico.c's -- both take it from the same -D on the command line.
 * The default was sized for the attract loop's 188 PCs; gameplay has 8,329 and
 * saturates it. See the note beside the table in sh2pico.c. */
#ifndef RIG_PC_HIST_SLOTS
#define RIG_PC_HIST_SLOTS 8192
#endif
struct rig_pc_slot {
    uint32_t pc;
    uint32_t occupied;
    uint16_t opcode;
    unsigned long long dir;
    unsigned long long dly;
    unsigned long long back; /* GNW: mirror of sh2pico.c slot layout */
};
extern struct rig_pc_slot rig_pchist[2][RIG_PC_HIST_SLOTS];
/* GNW: window-gated block census (team-lead directive: all PC/op/block
 * stats restricted to one fixed combat window; boot-inclusive figures
 * must never be quoted as combat bottleneck ratios). */
extern int rig_sh2_census_on;
extern unsigned long long rig_blklen[2][64];
extern unsigned long long rig_census_nins[2], rig_census_nback[2];

struct rig_pc_top { uint32_t pc; uint32_t core; uint16_t opcode; unsigned long long dir, dly, back, total; };

static int rig_pchist_cmp(const void *a, const void *b) {
    const struct rig_pc_top *x = a, *y = b;
    return (x->total < y->total) ? 1 : (x->total > y->total) ? -1 : 0;
}

static void rig_pchist_report(void) {
    struct rig_pc_top *top = (struct rig_pc_top *)malloc(sizeof(*top) * 2 * RIG_PC_HIST_SLOTS);
    if (!top) { printf("[32x-pchist] alloc fail\n"); return; }
    unsigned long long grand = 0;
    unsigned n = 0;
    for (int c = 0; c < 2; c++) {
        for (unsigned i = 0; i < RIG_PC_HIST_SLOTS; i++) {
            if (!rig_pchist[c][i].occupied) continue;
            unsigned long long d = rig_pchist[c][i].dir;
            unsigned long long dl = rig_pchist[c][i].dly;
            unsigned long long t = d + dl;
            if (t == 0) continue;
            top[n].pc = rig_pchist[c][i].pc;
            top[n].core = (uint32_t)c;
            top[n].opcode = rig_pchist[c][i].opcode;
            top[n].dir = d; top[n].dly = dl; top[n].back = rig_pchist[c][i].back;
            top[n].total = t;
            grand += t;
            n++;
        }
    }
    qsort(top, n, sizeof(*top), rig_pchist_cmp);
    unsigned shown = n < 50 ? n : 50;
    /* CDF: how many distinct guest PCs cover 50/90/95/99/100% of executions.
       The union of PCs is not what a dynarec code cache must hold; the hot
       prefix is. Translated size ~= covered_PCs * (host bytes per guest insn).
       Caveat, learned 2026-08-28: an execution-weighted CDF UNDERSTATES the
       working set, because it counts a PC once no matter how far apart its
       executions are. Use it to bound the problem, not to size a cache -- for
       that, ask a cache simulation. */
    {
        unsigned long long tot = 0, run = 0;
        for (unsigned i = 0; i < n; i++) tot += top[i].total;
        const double marks[] = {0.50, 0.90, 0.95, 0.99, 0.999};
        unsigned mi = 0;
        printf("[32x-pchist] CDF over %u PCs / %llu insns:\n", n, tot);
        for (unsigned i = 0; i < n && mi < 5; i++) {
            run += top[i].total;
            while (mi < 5 && (double)run >= marks[mi] * (double)tot) {
                printf("[32x-pchist]   %5.1f%% of executions <- %6u PCs"
                       " (%6u B guest, ~%6.1f KB @12B/insn)\n",
                       marks[mi]*100.0, i+1, (i+1)*2, (i+1)*12/1024.0);
                mi++;
            }
        }
        printf("[32x-pchist]   100.0%% of executions <- %6u PCs"
               " (%6u B guest, ~%6.1f KB @12B/insn)\n", n, n*2, n*12/1024.0);
    }
    printf("[32x-pchist] %u unique PCs, %llu total guest SH-2 insns; top %u:\n",
           n, grand, shown);
    {
        /* Occupancy and drops, always -- a share computed off a saturated
         * table is an under-count wearing a percentage sign, and the table
         * saturating is also what makes the instrument cost more than the
         * emulator it is measuring. */
        extern unsigned long long rig_pchist_dropped[2];
        unsigned long long dropped = rig_pchist_dropped[0] + rig_pchist_dropped[1];
        unsigned pct = (unsigned)((unsigned long long)n * 100 /
                                  (2u * RIG_PC_HIST_SLOTS));
        printf("[32x-pchist] table %u/%u slots (%u%%), dropped %llu insns%s\n",
               n, 2u * RIG_PC_HIST_SLOTS, pct, dropped,
               dropped ? "  <-- RAISE RIG_PC_HIST_SLOTS AND RE-RUN" : "");
    }
    unsigned long long acc = 0;
    for (unsigned i = 0; i < shown; i++) {
        acc += top[i].total;
        char dasm[80] = "";
        DasmSH2(dasm, top[i].pc, top[i].opcode);
        printf("[32x-pchist]  #%-2u %-6s 0x%08x  dir=%-10llu dly=%-10llu back=%-9llu %5.2f%%  cum %5.2f%%  %s\n",
               i + 1, top[i].core ? "slave" : "master", top[i].pc,
               top[i].dir, top[i].dly, top[i].back,
               grand ? 100.0 * top[i].total / grand : 0.0,
               grand ? 100.0 * acc / grand : 0.0, dasm);
    }
    /* GNW: window-gated loop census -- backedge count, mean run length,
     * and a basic-block-length histogram (index = sequential insns before
     * a backedge, clamped at 63). Loop-head candidates = slots where
     * back > 0. */
    for (int c = 0; c < 2; c++) {
        unsigned long long ni = rig_census_nins[c], nb = rig_census_nback[c];
        if (!ni) { printf("[32x-census] core %d: window empty\n", c); continue; }
        double mean_run = nb ? (double)ni / (double)nb : 0.0;
        printf("[32x-census] %-6s window insns=%llu backedges=%llu mean-run=%.1f backedge-every=%.1f insns\n",
               c ? "slave" : "master", ni, nb, mean_run,
               nb ? (double)ni / (double)nb : 0.0);
        unsigned long long bsum = 0;
        for (int k = 0; k < 64; k++) bsum += rig_blklen[c][k];
        if (bsum) {
            printf("[32x-census] %-6s blklen hist (insns before backedge):",
                   c ? "slave" : "master");
            for (int k = 0; k < 64; k++) {
                if (!rig_blklen[c][k]) continue;
                if ((k % 8) == 0) printf("\n[32x-census]   len%02d-%02d:", k, k + 7 < 63 ? k + 7 : 63);
                printf(" %llu(%.1f%%)", rig_blklen[c][k],
                       100.0 * (double)rig_blklen[c][k] / (double)bsum);
            }
            printf("\n");
        }
    }
    free(top);
}
#endif /* RIG_SH2_PC_HIST */

#ifdef RIG_Z80_HIST
#ifndef RIG_Z80_HIST_FROM
#define RIG_Z80_HIST_FROM 0
#endif
extern unsigned long long g_z80_pc_hist[0x10000];
extern unsigned long long g_z80_insns;
static void rig_z80_pchist_report(void) {
    /* Top guest PCs of the Z80 driver over the counted window. Selection
     * loop instead of qsort: 48 passes over 64K counters is nothing on a
     * host and it destroys the table as it goes, which is fine -- this
     * runs once, last, after every other consumer of the run. */
    unsigned long long tot = 0;
    for (unsigned a = 0; a < 0x10000; a++) tot += g_z80_pc_hist[a];
    printf("[32x-z80pc] window insns=%llu\n", g_z80_insns);
    /* Full dump BEFORE the destructive top-48 selection below: the engine
     * runs once per frame, so its addresses sit far under the spin noise
     * and a top-N view answers "where does the spin live" -- which we
     * already know -- instead of "where does the work live". */
    for (unsigned a = 0; a < 0x10000; a++)
        if (g_z80_pc_hist[a])
            printf("[32x-z80pc-full] 0x%04x %llu\n", a, g_z80_pc_hist[a]);
    for (unsigned b = 0; b < 256; b++) {
        unsigned long long s = 0;
        for (unsigned i = 0; i < 256; i++) s += g_z80_pc_hist[(b << 8) | i];
        if (s) printf("[32x-z80pc-bucket] 0x%02x00 %llu %5.2f%%\n",
                      b, s, tot ? 100.0 * s / tot : 0.0);
    }
    if (!tot) { printf("[32x-z80pc] empty table\n"); return; }
    unsigned long long cum = 0;
    for (int rank = 1; rank <= 48; rank++) {
        unsigned best = 0;
        unsigned long long bc = 0;
        for (unsigned a = 0; a < 0x10000; a++)
            if (g_z80_pc_hist[a] > bc) { bc = g_z80_pc_hist[a]; best = a; }
        if (!bc) break;
        cum += bc;
        printf("[32x-z80pc] #%-2u 0x%04x %10llu %5.2f%% cum %5.2f%%\n",
               rank, best, bc, 100.0 * bc / tot, 100.0 * cum / tot);
        g_z80_pc_hist[best] = 0;
    }
}
#endif /* RIG_Z80_HIST */

#ifdef RIG_YM_CENSUS
#ifndef RIG_YM_CENSUS_FROM
#define RIG_YM_CENSUS_FROM 311
#endif
extern void ym_census_frame(void);
extern void ym_census_report(void);
#endif

#ifdef RIG_POLL_PEEK
/* Diagnostic: prints every distinct backward-branch site (BF/BFS/BT/BTS with
 * negative disp8) discovered by sh2pico.c's RIG_POLL_PEEK hook, with the full
 * register snapshot taken on first visit. The "target" is the loop top
 * (ppc + disp8*2 + 4); the body between target and ppc is the poll loop body,
 * and the poll address is r[base_reg] (for MOV.W/L @Rn forms) or gbr+disp
 * (for @(disp,GBR) forms). Region = top byte of poll_addr: 0x06=SDRAM,
 * 0x00/0x20=CS0 sysreg (comm/VDP/H-count/PWM), 0x02/0x22=cart ROM,
 * 0x04/0x24=DRAM, 0xC0=data array, 0xFF=peripheral. Resolves which region
 * each no-match ROM spins on. */
struct rig_peek_entry {
    uint32_t pc;
    uint16_t op;
    int core;
    uint32_t r[16];
    uint32_t gbr, vbr, sr;
};
extern struct rig_peek_entry rig_peek_log[128];
extern int rig_peek_n;

static const char *rig_peek_region(uint32_t a) {
    switch ((a >> 24) & 0xff) {
        case 0x06: return "SDRAM";
        case 0x00: case 0x20: return "CS0-sysreg";
        case 0x02: case 0x22: return "cart-ROM";
        case 0x04: case 0x24: return "DRAM";
        case 0xc0: return "data-array";
        case 0xff: return "periph";
        default: return "other";
    }
}

static void rig_peek_report(void) {
    printf("[32x-peek] %d unique backward-branch sites:\n", rig_peek_n);
    for (int i = 0; i < rig_peek_n; i++) {
        struct rig_peek_entry *e = &rig_peek_log[i];
        int disp8 = (int)(signed char)(e->op & 0xff);
        uint32_t target = e->pc + disp8 * 2 + 4;
        printf("[32x-peek]  #%d pc=%08x core=%c op=%04x gbr=%08x target=%08x\n",
               i, e->pc, e->core ? 'S' : 'M', e->op, e->gbr, target);
        printf("[32x-peek]    R0=%08x R1=%08x R2=%08x R3=%08x  R4=%08x R5=%08x R6=%08x R7=%08x\n",
               e->r[0], e->r[1], e->r[2], e->r[3], e->r[4], e->r[5], e->r[6], e->r[7]);
        printf("[32x-peek]    R8=%08x R9=%08x R10=%08x R11=%08x R12=%08x R13=%08x R14=%08x R15=%08x\n",
               e->r[8], e->r[9], e->r[10], e->r[11], e->r[12], e->r[13], e->r[14], e->r[15]);
    }
}
#endif /* RIG_POLL_PEEK */

#ifdef RIG_SDRAM_POLL_DIAG
/* Diagnostic for the SDRAM poll case of gnw_sh2_fastloop: counts how often the
 * case is entered (tries), how often it matches a real SDRAM poll (hits), and
 * why it rejects the rest (bad body opcodes / non-SDRAM poll address), with up
 * to 64 samples. Off => byte-identical. */
struct rig_spd_sample { uint32_t pc, bop1, bop2, pa; };
extern struct rig_spd_sample rig_spd_log[];
extern volatile uint32_t rig_spd_tries, rig_spd_hits;
extern volatile uint32_t rig_spd_bad_bop, rig_spd_bad_addr;
extern volatile uint32_t rig_spd_addr_06, rig_spd_addr_00, rig_spd_addr_02;
extern volatile uint32_t rig_spd_addr_22, rig_spd_addr_40, rig_spd_addr_other;
extern volatile uint32_t rig_spd_log_n;

static void rig_spd_report(void) {
    printf("[32x-spd] tries=%u hits=%u  bad_bop=%u  bad_addr=%u\n",
           rig_spd_tries, rig_spd_hits, rig_spd_bad_bop, rig_spd_bad_addr);
    printf("[32x-spd] bad_addr by region: 06(sdram)=%u 00/20(cs0)=%u "
           "02/22(rom)=%u 22(dram)=%u 40(comm)=%u other=%u\n",
           rig_spd_addr_06, rig_spd_addr_00, rig_spd_addr_02,
           rig_spd_addr_22, rig_spd_addr_40, rig_spd_addr_other);
    uint32_t n = rig_spd_log_n;
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) {
        printf("[32x-spd]  #%u pc=%08x bop1=%04x bop2=%04x pa=%08x\n",
               i, rig_spd_log[i].pc, rig_spd_log[i].bop1,
               rig_spd_log[i].bop2, rig_spd_log[i].pa);
    }
}
#endif /* RIG_SDRAM_POLL_DIAG */

/* ==== video ================================================================ */
static uint16_t s_fb[320 * 240];
static int out_line = (240 - 224) / 2;
static int out_col  = 0;

static void set_out_buffer(void) {
    PicoDrawSetOutBuf(s_fb + out_line * 320 + out_col, 320 * 2);
}

/* Fired by the LAZY Pico32xStartup (the game's 68K writes ADEN at 0xA15101).
 * PicoDrawSetOutFormat / PicoDrawSetOutBuf route to their 32X variants only
 * once PAHW_32X is set, so they MUST be re-applied here — otherwise the 32X
 * layer renders into picodrive's internal DefOutBuff and the screen stays
 * black (libretro's emu_32x_startup does exactly this re-apply). */
void emu_32x_startup(void) {
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    set_out_buffer();
#ifdef GNW_SH2_DRC
#ifdef GNW_SH2_DRC
    /* Rig DRC enablement. Historically gated by GNW_DRC_RIG (so the s3
     * C/D arms could run DRC-free), which had a side effect: a build with
     * ONLY -DGNW_SH2_DRC (the production flag the device arm uses) never
     * turned the translator on — lookups=0, +6.8% pure hook-check tax
     * (measured 2026-09-26, ab_drc_prod.log). Per team lead: report real
     * coverage for the candidate ELF — so plain GNW_SH2_DRC now enables
     * too. The rig_tbudget=30000 translate throttle in sh2pico.c stays
     * GNW_DRC_RIG-only (diagnostic; NOT in production or device builds). */
    {
        extern void gnw_sh2_drc_attach(unsigned char *, unsigned);
        extern unsigned long long gnw_sh2_drc_lookups, gnw_sh2_drc_misses,
                gnw_sh2_drc_runs, gnw_sh2_drc_trans, gnw_sh2_drc_flushes;
        extern int gnw_sh2_drc_on;
        static unsigned char rig_drc_pool[16 * 1024] __attribute__((aligned(8)));
        /* GNW bug-28 test: kill the accidental-BSS-zero condition. Poison the
         * pool first, then run the REAL production init path (attach), and
         * verify idx[] came out fully invalid plus both end canaries cleared.
         * This mirrors the device fault (stale AHB idx -> UNALIGNED at
         * run+0x34). rig-only; device uses the same attach() in main_md32x.c */
        {
            unsigned i, bad = 0;
            unsigned short *idx = (unsigned short *)(void *)rig_drc_pool;
            unsigned *w = (unsigned *)(void *)rig_drc_pool;
            memset(rig_drc_pool, 0xa5, sizeof(rig_drc_pool));
            gnw_sh2_drc_attach(rig_drc_pool, sizeof(rig_drc_pool));
            for (i = 0; i < 1024; i++) if (idx[i]) bad++;
            printf("[drc] attach prefill check: bad_idx=%u canary_first=%08x canary_last=%08x (want 0/0/0)\n",
                   bad, w[0], w[sizeof(rig_drc_pool)/4 - 1]);
        }
        gnw_sh2_drc_on = 1;
    }
#endif
#endif
#ifdef GNW_S3_ARM
    /* team-lead s3b separation: 0=oracle (fold off, probe on),
     * 1=C (fold on, re-read every iteration), 2=D (fold on, single
     * read + arithmetic). Reported at exit via gnw_sh2_s3_folds. */
    {
        extern int gnw_sh2_s3_off, gnw_sh2_s3_once;
        gnw_sh2_s3_off  = (GNW_S3_ARM == 0);
        gnw_sh2_s3_once = (GNW_S3_ARM == 2);
        printf("[s3] arm=%d off=%d once=%d\n",
               GNW_S3_ARM, gnw_sh2_s3_off, gnw_sh2_s3_once);
    }
#endif
}
void emu_video_mode_change(int start_line, int line_count, int start_col, int col_count) {
    (void)start_line; (void)start_col;
    out_line = (240 - line_count) / 2; if (out_line < 0) out_line = 0;
    out_col  = (320 - col_count)  / 2; if (out_col  < 0) out_col  = 0;
    set_out_buffer();
}

static uint32_t fb_checksum(int *nonblank) {
    uint32_t sum = 0; uint32_t nz = 0;
    for (int i = 0; i < 320 * 240; i++) { sum = sum * 131 + s_fb[i]; nz |= s_fb[i]; }
    *nonblank = nz != 0;
    return sum;
}

#ifdef RIG_FB_DUMP
/* Print the framebuffer, run-length encoded, at one frame. A frozen screen is
 * usually a message -- an allocator failure, an assertion, a "press start" --
 * and a 32-bit checksum cannot tell those apart. This exists because guessing
 * which one it was from PC histograms alone burned real time. Reconstruct with
 * tools/m7_qemu_rig/fbdump_to_png.py.
 *
 * RLE because there is no file I/O here: SYS_OPEN returns -1 under this
 * runtime, printf is all we have, and an error screen is nearly all one
 * colour, so the whole 320x240 costs a few hundred lines. */
static void fb_dump_rle(int frame) {
    printf("[fbd] frame=%d w=320 h=240\n", frame);
    int i = 0;
    while (i < 320 * 240) {
        unsigned short v = s_fb[i];
        int n = 1;
        while (i + n < 320 * 240 && s_fb[i + n] == v && n < 65535) n++;
        printf("[fbd] %d %04x\n", n, v);
        i += n;
    }
    printf("[fbd] end\n");
}
#endif

/* ==== audio ================================================================ */
/* mono like the device (no POPT_EN_STEREO); nonzero sndRate is REQUIRED (it
 * sets pwm.cycles — with 0 the PWM scheduler spins the frame forever) */
static short s_snd[4096];
static unsigned s_snd_calls, s_snd_samples;

/* Running hash of every sample the core produces.
 *
 * The framebuffer hash cannot hear. The last time an SH-2 spin-skip was tried
 * on this core it was reverted because it broke Doom's gunshot PWM effect --
 * found by ear, on the device, after the rig had already called the change
 * clean. Any lever that changes WHEN the SH-2s run can change what the sound
 * chips are fed, so a spin-skip A/B is only meaningful if both arms hash the
 * same audio as well as the same pixels. */
static uint32_t s_snd_hash = 2166136261u;      /* FNV-1a 32 */

/* Energy alongside the hash, because the hash answers only "identical or
 * not" and some levers move sound WITHOUT changing it in any way a listener
 * could hear. The 68K idle fold is the case that forced this: it takes the
 * VInt handler up to one scanline later, which shifts every sample boundary
 * downstream, so the hash moves on audio that is otherwise the same content.
 * Sum|s| and sum s^2 survive a small time shift and do not survive a real
 * change of what is being played, so the pair separates "rescheduled" from
 * "broken". Neither number replaces the hash: an unmoved hash is still the
 * only proof of bit-identity. */
static unsigned long long s_snd_abs, s_snd_sq;
static int s_snd_peak;

extern int rig_frame_idx; /* defined below (frame loop) */
static void rig_write_sound(int len)
{
    const unsigned char *p = (const unsigned char *)s_snd;
    int nbytes = len * (int)sizeof(s_snd[0]);
    int i;

    s_snd_calls++;
    s_snd_samples += (unsigned)len;
#ifdef RIG_SND_CKS
    /* GNW: per-call cumulative snd hash — A/B diff pinpoints the
     * first diverging audio frame (snd-only divergence isolation). */
    printf("sck c%04d f%04d=%08x\n", s_snd_calls, rig_frame_idx, s_snd_hash);
#endif
    if (nbytes > (int)sizeof(s_snd)) nbytes = (int)sizeof(s_snd);
    for (i = 0; i < nbytes; i++) {
        s_snd_hash ^= p[i];
        s_snd_hash *= 16777619u;
    }
    if (len > (int)(sizeof(s_snd) / sizeof(s_snd[0])))
        len = (int)(sizeof(s_snd) / sizeof(s_snd[0]));
    for (i = 0; i < len; i++) {
        int v = s_snd[i];
        int a = v < 0 ? -v : v;
        s_snd_abs += (unsigned)a;
        s_snd_sq  += (unsigned long long)((long long)v * v);
        if (a > s_snd_peak) s_snd_peak = a;
    }
}

/* ==== optional-mode helpers =============================================== */

/* PicoIn.pad format: MXYZ SACB RLDU (pico.h) */
/* Frame at which RIG_STATE_LOAD resumes the device savestate. Declared here
 * because pad_script below dates its movement from it. */
#ifndef RIG_STATE_LOAD_AT
#define RIG_STATE_LOAD_AT 60
#endif

#define PAD_UP    (1u << 0)
#define PAD_DOWN  (1u << 1)
#define PAD_LEFT  (1u << 2)
#define PAD_RIGHT (1u << 3)
#define PAD_B     (1u << 4)
#define PAD_C     (1u << 5)
#define PAD_A     (1u << 6)
#define PAD_START (1u << 7)

#ifdef RIG_PAD_SCRIPT
/* Plausible VF session: START held around f120 (title/menu advance), a second
 * START window for the next screen, then directional+button mashing. */
static unsigned short pad_script(int f) {
#ifdef RIG_STATE_LOAD
    /* A resumed savestate is already IN the level -- there is no title to walk,
     * and the walker below would press A on a live player. Move instead, from
     * the load frame on: standing still with only fire held renders a handful
     * of frames and then repeats, which reads as a frozen screen and profiles
     * as one. Deterministic, so two arms stay comparable. */
    int g = f - RIG_STATE_LOAD_AT;
    if (g < 0) return 0;              /* pre-load frames: hands off */
    switch ((g / 30) % 4) {
    case 0:  return PAD_UP;
    case 1:  return PAD_UP | PAD_RIGHT;
    case 2:  return PAD_UP | PAD_LEFT;
    default: return PAD_UP | PAD_C;
    }
#else
    /* Two START presses only ever reached a menu: profiling f300-f395 with the
     * old script produced the same top guest PCs as the attract loop (a delay
     * loop and a GBR getter, no renderer anywhere), so the rig had never once
     * measured Doom actually drawing a level. Doom's title needs START, then a
     * selection, then a difficulty confirm, and any one of them landing on the
     * wrong frame leaves you in a menu that looks like progress.
     *
     * So: alternate START and A every 40 frames until f400 instead of guessing
     * the timing, then walk. RIG_PAD_LEVEL_FROM moves where movement begins. */
#ifndef RIG_PAD_LEVEL_FROM
#define RIG_PAD_LEVEL_FROM 400
#endif
    /* Phase 1 (to f300): alternate START and A to walk the title into the main
     * menu. Phase 2 (f320-330): ONE A press to commit the highlighted START
     * entry. Phase 3: hands off until RIG_PAD_LEVEL_FROM, because continuing to
     * press re-enters the menu -- which is what the alternating script did, and
     * why a dump at f520 showed the menu sitting on top of a rendered level
     * instead of a level. */
    /* Minimal and deterministic. Doom's START is also the pause/menu toggle, so
     * a script that keeps pressing cycles the front end forever: an earlier
     * version alternated START and A every 20 frames and a dump at f430 showed
     * the TITLE screen again, having passed through the menu twice. One press
     * to leave the title, one to commit the highlighted entry, then hands off. */
    /* Doom's START is also the pause/menu toggle, so a script that keeps
     * pressing cycles the front end: alternating START and A landed on the
     * TITLE again at f430, and pressing exactly twice never left it at all.
     * What this does reach, and hold, is the main menu drawn ON TOP of a
     * rendered level -- 121 colours and 94.5k guest SH-2 insn/frame against the
     * attract loop's 69.9k. The renderer is running there, which is what a
     * profile of the renderer needs; it is the best gameplay proxy the rig has
     * until it can resume the device savestate. */
    /* Mash A. Doom's START is also the pause/menu toggle, so any script that
     * keeps pressing START cycles the front end -- alternating START and A
     * landed back on the TITLE at f430, and pressing exactly twice never left
     * it. A alone confirms without ever re-opening the menu. */
    if (f < 400) return (f % 12) < 6 ? PAD_A : 0;
    if (f < RIG_PAD_LEVEL_FROM) return 0;
    if (f >= RIG_PAD_LEVEL_FROM) {
        switch ((f / 8) % 6) {
        case 0:  return PAD_RIGHT;
        case 1:  return PAD_RIGHT | PAD_B;
        case 2:  return PAD_LEFT;
        case 3:  return PAD_LEFT | PAD_C;
        case 4:  return PAD_A;
        default: return PAD_DOWN | PAD_B;
        }
    }
    return 0;
#endif /* RIG_STATE_LOAD */
}
#else
static unsigned short pad_script(int f) { (void)f; return 0; }
#endif

#if defined(RIG_STATE_TEST) || defined(RIG_STATE_LOAD)
/* A seekable byte stream over memory, in picodrive's area-I/O shape. There is
 * no file I/O in this runtime: semihosting SYS_OPEN returns -1 here (proven by
 * two failed probe runs; SYS_WRITE0/printf work), so both the round-trip test
 * and the device-state loader below run the state through RAM. */
struct rig_state_stream {
    unsigned char *data;
    size_t capacity;
    size_t length;
    size_t position;
};

static size_t rig_state_read(void *ptr, size_t size, size_t count, void *opaque) {
    struct rig_state_stream *stream = opaque;
    if (size == 0) return 0;
    size_t available = (stream->length - stream->position) / size;
    if (count > available) count = available;
    memcpy(ptr, stream->data + stream->position, size * count);
    stream->position += size * count;
    return count;
}

static size_t rig_state_write(void *ptr, size_t size, size_t count, void *opaque) {
    struct rig_state_stream *stream = opaque;
    if (size != 0 && count > (stream->capacity - stream->position) / size)
        return 0;
    memcpy(stream->data + stream->position, ptr, size * count);
    stream->position += size * count;
    if (stream->length < stream->position) stream->length = stream->position;
    return count;
}

static size_t rig_state_eof(void *opaque) {
    struct rig_state_stream *stream = opaque;
    return stream->position >= stream->length;
}

static int rig_state_seek(void *opaque, long offset, int whence) {
    struct rig_state_stream *stream = opaque;
    long base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (long)stream->position;
    else if (whence == SEEK_END) base = (long)stream->length;
    else return -1;
    long position = base + offset;
    if (position < 0 || (size_t)position > stream->length) return -1;
    stream->position = (size_t)position;
    return 0;
}

#endif /* RIG_STATE_TEST || RIG_STATE_LOAD */

#ifdef RIG_STATE_TEST
#define RIG_STATE_CAP (1024u * 1024u)

/* State round-trip test: warm up to a stable non-blank state, save, run N
 * frames forward, capture fb; load (restore), re-run the same N frames,
 * capture fb; PASS iff both non-blank and identical.  The warm-up and test
 * windows are chosen to avoid game-specific blank transitions (Doom 32X
 * blanks around f140-f150 during a loading screen, so we test in f50-f80
 * where the framebuffer is stably non-blank). */
#ifndef RIG_STATE_WARMUP
#define RIG_STATE_WARMUP 50
#endif
#ifndef RIG_STATE_TESTFRAMES
#define RIG_STATE_TESTFRAMES 30
#endif

static int state_roundtrip_test(void) {
    struct rig_state_stream stream = {
        .data = malloc(RIG_STATE_CAP),
        .capacity = RIG_STATE_CAP,
    };
    if (stream.data == NULL) {
        printf("[32x-state] FAIL: buffer allocation\n");
        return -1;
    }

    for (int f = 0; f < RIG_STATE_WARMUP; f++) {
        PicoIn.skipFrame = 0;
        PicoIn.pad[0] = pad_script(f);
        PicoFrame();
    }
    int save_ret = PicoStateFP(&stream, 1, rig_state_read, rig_state_write,
                               rig_state_eof, rig_state_seek);

    const int f0 = RIG_STATE_WARMUP;
    const int f1 = RIG_STATE_WARMUP + RIG_STATE_TESTFRAMES;
    for (int f = f0; f < f1; f++) {
        PicoIn.skipFrame = 0;
        PicoIn.pad[0] = pad_script(f);
        PicoFrame();
    }
    int nonblank_a;
    uint32_t checksum_a = fb_checksum(&nonblank_a);

    stream.position = 0;
    int load_ret = PicoStateFP(&stream, 0, rig_state_read, rig_state_write,
                               rig_state_eof, rig_state_seek);
    set_out_buffer();
    for (int f = f0; f < f1; f++) {
        PicoIn.skipFrame = 0;
        PicoIn.pad[0] = pad_script(f);
        PicoFrame();
    }
    int nonblank_b;
    uint32_t checksum_b = fb_checksum(&nonblank_b);

    int pass = save_ret == 0 && load_ret == 0 && nonblank_a && nonblank_b &&
               checksum_a == checksum_b;
    printf("[32x-state] %s: bytes=%lu save=%d load=%d checksum=%08lx/%08lx AHW=%x\n",
           pass ? "PASS" : "FAIL", (unsigned long)stream.length, save_ret, load_ret,
           (unsigned long)checksum_a, (unsigned long)checksum_b,
           (unsigned)PicoIn.AHW);
    free(stream.data);
    return pass ? 0 : -1;
}
#endif /* RIG_STATE_TEST */

#ifdef RIG_STATE_LOAD
/* ---- resume the device's own savestate ------------------------------------
 * The .sav is linked in as a blob (run_32x.sh: RIG_32X_STATE=<file>), because
 * this runtime has no file I/O -- same route the cart takes.
 *
 * The firmware wraps picodrive's stream in an 8-byte header (MD32X_STATE_MAGIC
 * "X2XM" + a version word); see md32x_SaveState in
 * Core/Src/porting/md32x/main_md32x.c. Skip it and hand the rest to
 * PicoStateFP, exactly as md32x_LoadState does.
 *
 * WHY THIS LIVES IN THE RIG AND NOT IN tools/pico_host: the stream is a raw
 * dump of live structs, so it carries the DEVICE'S POINTER WIDTHS. A 64-bit
 * host build stops dead at
 *     unexpected len 4, wanted 8 (sizeof(Pico32xMem->m68k_rom))
 * because m68k_rom is a pointer. This rig is 32-bit ARM and is the device's
 * word size exactly, so the same eight-byte skip just works here.
 *
 * It must run AFTER the adapter has started. Pico32xStartup is LAZY -- the
 * game's own 68K writes ADEN at 0xA15101 -- and the 32X chunks read straight
 * into Pico32xMem, so loading at frame 0 would write through a null pointer.
 * Hence RIG_STATE_LOAD_AT, and the two guards below that refuse rather than
 * fault if the adapter is not up yet. */
extern const unsigned char _binary_state_bin_start[];
extern const unsigned char _binary_state_bin_end[];

#define MD32X_STATE_MAGIC   0x4D583258u   /* "X2XM" on the wire */
#define MD32X_STATE_VERSION 3
#define MD32X_STATE_HDR     16u
/* v3 (fbfa50e6): {magic, version, bytes, crc32} -- the length word lets us
 * cross-check the blob before streaming a single guest byte. */

struct Pico32xMem;                        /* opaque: only the null test matters */
extern struct Pico32xMem *Pico32xMem;

static int rig_load_device_state(void) {
    const unsigned char *blob = _binary_state_bin_start;
    size_t blob_len = (size_t)(_binary_state_bin_end - _binary_state_bin_start);
    uint32_t magic = 0, version = 0;

    if (blob_len <= MD32X_STATE_HDR) {
        printf("[32x-state] FAIL: state blob is %lu bytes\n", (unsigned long)blob_len);
        return -1;
    }
    memcpy(&magic, blob, 4);
    memcpy(&version, blob + 4, 4);
    printf("[32x-state] blob=%lu bytes magic=%08lx ver=%lu\n",
           (unsigned long)blob_len, (unsigned long)magic, (unsigned long)version);
    if (magic != MD32X_STATE_MAGIC) {
        printf("[32x-state] FAIL: bad magic (want %08lx)\n",
               (unsigned long)MD32X_STATE_MAGIC);
        return -1;
    }
    if (version != MD32X_STATE_VERSION) {
        printf("[32x-state] FAIL: version %lu, this build writes %u\n",
               (unsigned long)version, MD32X_STATE_VERSION);
        return -1;
    }
    uint32_t pay_bytes = 0;
    memcpy(&pay_bytes, blob + 8, 4);
    if ((size_t)MD32X_STATE_HDR + pay_bytes != blob_len) {
        printf("[32x-state] FAIL: length word %lu + hdr != blob %lu\n",
               (unsigned long)pay_bytes, (unsigned long)blob_len);
        return -1;
    }
    if (!(PicoIn.AHW & PAHW_32X) || Pico32xMem == NULL) {
        printf("[32x-state] FAIL: 32X not started yet (AHW=%x mem=%p) -- "
               "raise RIG_STATE_LOAD_AT past the game's ADEN write\n",
               (unsigned)PicoIn.AHW, (void *)Pico32xMem);
        return -1;
    }

    struct rig_state_stream stream = {
        .data     = (unsigned char *)blob + MD32X_STATE_HDR,
        .capacity = blob_len - MD32X_STATE_HDR,
        .length   = blob_len - MD32X_STATE_HDR,
        .position = 0,
    };
    int rc = PicoStateFP(&stream, 0, rig_state_read, rig_state_write,
                         rig_state_eof, rig_state_seek);
    /* A load restores state, not the things derived from it: the device does
     * this same re-point in md32x_LoadState, because PicoDrawSetOutBuf's 32X
     * routing is re-established per load. */
    set_out_buffer();

    int nonblank = 0;
    uint32_t cks = fb_checksum(&nonblank);
    printf("[32x-state] %s: PicoStateFP=%d consumed=%lu/%lu fb=%08lx nonblank=%d\n",
           rc == 0 ? "LOADED" : "FAIL", rc,
           (unsigned long)stream.position, (unsigned long)stream.length,
           (unsigned long)cks, nonblank);
    return rc;
}
#endif /* RIG_STATE_LOAD */

#ifdef RIG_TRACE_PC
/* peek 68K memory the way the g68k core does (I/O handler or direct base) —
 * lets the trace dump the supervisor stack, so a parked "unexpected
 * exception" loop still names its culprit via the stacked SR/PC frame */
static unsigned rig_m68k_peek16(unsigned a) {
    const cpu_memory_map *mm = &m68k.memory_map[(a >> 16) & 0xff];
    if (mm->read16) return mm->read16(a) & 0xffff;
    if (mm->base)   return *(const unsigned short *)(mm->base + (a & 0xfffe));
    return 0xdead;
}
#endif

#ifdef RIG_SKIP3
/* draw 1 of every 3 frames — the device's frame_loop under sustained load */
static int skip_this_frame(int f) { return (f % 3) != 0; }
/* checksum checkpoints must land on DRAWN frames */
#define CK_LAST (RIG_FRAMES - 1 - ((RIG_FRAMES - 1) % 3))
#define CK_A 99
#define CK_B 300
#else
static int skip_this_frame(int f) { (void)f; return 0; }
#define CK_LAST (RIG_FRAMES - 1)
#define CK_A 99
#define CK_B 299
#endif

/* ==== main ================================================================= */
int rig_frame_idx = -1;   /* GNW: current frame, read by sh2pico DRC probes */

#ifdef RIG_S3_HARNESS
/* GNW: shape3 differential-verification harness (team-lead m4301 item 4).
 * Replicates identical initial CPU state / memory / slice budget, then runs
 * the pure interpreter (oracle, s3_off=1) and the fold arms (D = production
 * single-read arithmetic fold via s3_once=1; C = DIAGNOSTIC volatile re-read
 * loop via s3_once=0, requires GNW_S3_DIAG so the loop survives codegen) to
 * the SAME termination boundary (DT exhaustion / immediate byte exit — both
 * end parked in a BRA-self spin, so the state is stable to compare).
 * Compares r[16], pc, ppc, pr, sr, t_flag, gbr, vbr, mach, macl, delay,
 * test_irq and the whole SDRAM window; icount is checked against the exact
 * overcharge rule (D consumes 7 cycles per folded iteration, the interpreter
 * 5 -> residual_D == residual_O - 2*sum(k)). `ea` is excluded: it is
 * non-architectural handler scratch (documented at the DRC design). Budget
 * -bounded cases (slice/unsigned-large) cannot share an event boundary with
 * the oracle and are checked against the analytic spec instead — recorded
 * per case with the ACTUAL accumulated iteration counts (never a threshold
 * label). No mid-run writer exists on a single core: mid-fold byte flips are
 * structurally covered by the C-arm volatile re-read and empirically by the
 * full-boot fb/snd hash equality; that gap is recorded in the verdict. */
extern int gnw_sh2_s3_off, gnw_sh2_s3_once, gnw_sh2_fastloops;
extern unsigned long long gnw_sh2_s3_folds;
extern int sh2_execute_interpreter(SH2 *sh2, int cycles);

static unsigned char h_sd[0x40000];   /* physical ^1-swapped SDRAM image */
static SH2 h_sh2;

#define H_L   0x06000200u             /* loop head guest address */
#define H_OFF 0x200u
#define H_PA  0x06000240u             /* mailbox byte (guest) */

static void h_place_loop(unsigned bop1, unsigned bop4_dt)
{
    /* physical words are stored host-LE so the fast path reads logical BE */
    volatile unsigned short *w = (unsigned short *)(void *)h_sd;
    w[(H_OFF + 0) >> 1] = bop1;     /* MOV.B @(d,Rm),R0 (case-settable) */
    w[(H_OFF + 2) >> 1] = 0x2008;   /* TST R0,R0 */
    w[(H_OFF + 4) >> 1] = 0x8b02;   /* BF +2 -> byte-exit at L+12 */
    w[(H_OFF + 6) >> 1] = bop4_dt;  /* DT Rn (R2 normal / R1 alias case) */
    w[(H_OFF + 8) >> 1] = 0x8bfa;   /* BF -6 -> back to L+0 */
    w[(H_OFF + 10) >> 1] = 0x0009;  /* NOP (DT-exit fallthrough) */
    w[(H_OFF + 12) >> 1] = 0x0009;  /* NOP (byte-exit lands here) */
    w[(H_OFF + 14) >> 1] = 0xaffe;  /* BRA self (disp12=-2) spin park */
    w[(H_OFF + 16) >> 1] = 0x0009;  /* NOP delay slot (required pair) */
}

static void h_reset(unsigned v, unsigned char byte, unsigned bop1,
                    unsigned bop4_dt, unsigned r1, unsigned r2)
{
    memset(&h_sh2, 0, sizeof h_sh2);
    memset(h_sd, 0, sizeof h_sd);
    h_place_loop(bop1, bop4_dt);
    h_sd[(H_PA & 0x3ffffu) ^ 1u] = byte;
    h_sh2.p_sdram = h_sd;
    h_sh2.pc = H_L;
    h_sh2.ppc = H_L;
    h_sh2.sr = 0xf0;
    h_sh2.r[0] = 0xdeadbeefu;
    h_sh2.r[1] = r1;                /* default loop: ea = r1 + 1 */
    h_sh2.r[2] = r2;
    { int i; for (i = 3; i < 16; i++) h_sh2.r[i] = 0xc0000000u + i; }
    gnw_sh2_s3_folds = 0;
}

/* SDRAM equality across arms: the loop bodies under test contain no
 * stores, so the whole guest SDRAM image must be bit-identical between
 * arms of the same case. Compared over the full 256K h_sd image. */
static unsigned char h_sd_ref[0x40000];
static int h_mem_cmp(const char *tag, int first)
{
    if (first) { memcpy(h_sd_ref, h_sd, sizeof h_sd_ref); return 0; }
    if (memcmp(h_sd_ref, h_sd, sizeof h_sd_ref) != 0) {
        printf("[%s] FAIL SDRAM image differs from oracle arm\n", tag);
        return 1;
    }
    return 0;
}

static int h_state_cmp(const char *tag, const SH2 *a, const SH2 *b,
                       int ic_expect_diff, int *ic_diff_out)
{
    int i, bad = 0;
    if (a->pc != b->pc) { printf("[%s] FAIL pc %08x!=%08x\n", tag, a->pc, b->pc); bad = 1; }
    if (a->ppc != b->ppc) { printf("[%s] FAIL ppc %08x!=%08x\n", tag, a->ppc, b->ppc); bad = 1; }
    if (a->pr != b->pr) { printf("[%s] FAIL pr\n", tag); bad = 1; }
    if (a->sr != b->sr) { printf("[%s] FAIL sr %04x!=%04x\n", tag, a->sr&0xffff, b->sr&0xffff); bad = 1; }
    if (a->t_flag != b->t_flag) { printf("[%s] FAIL t %d!=%d\n", tag, a->t_flag, b->t_flag); bad = 1; }
    if (a->gbr != b->gbr || a->vbr != b->vbr) { printf("[%s] FAIL gbr/vbr\n", tag); bad = 1; }
    if (a->mach != b->mach || a->macl != b->macl) { printf("[%s] FAIL mac\n", tag); bad = 1; }
    if (a->delay != b->delay) { printf("[%s] FAIL delay %x!=%x\n", tag, a->delay, b->delay); bad = 1; }
    if (a->test_irq != b->test_irq) { printf("[%s] FAIL test_irq\n", tag); bad = 1; }
    for (i = 0; i < 16; i++)
        if (a->r[i] != b->r[i]) {
            printf("[%s] FAIL r[%d] %08x!=%08x\n", tag, i, a->r[i], b->r[i]);
            bad = 1;
        }
    *ic_diff_out = (int)a->icount - (int)b->icount;
    if (ic_expect_diff != 0x7fffffff && *ic_diff_out != ic_expect_diff) {
        printf("[%s] FAIL ic residual diff %d != expected %d "
               "(ic_a=%d ic_b=%d)\n", tag, *ic_diff_out, ic_expect_diff,
               (int)a->icount, (int)b->icount);
        bad = 1;
    }
    return bad;
}

/* run one arm; returns final state via h_sh2, folded iters via *fk */
static void h_run(unsigned off, unsigned once, int budget, unsigned *fk)
{
    unsigned save_r[16]; int i;
    extern void gnw_sh2_dlrej_clear(void);
    for (i = 0; i < 16; i++) save_r[i] = h_sh2.r[i];
    { unsigned sv_pc = h_sh2.pc, sv_sr = h_sh2.sr;
      /* oracle rejection poisons the shared DL_REJ slot for this ppc —
       * the fold arms must reach the matcher themselves */
      gnw_sh2_dlrej_clear();
      gnw_sh2_s3_off = off; gnw_sh2_s3_once = once;
      h_sh2.icount = 0;
      sh2_execute_interpreter(&h_sh2, budget);
      (void)sv_pc; (void)sv_sr; }
    *fk = (unsigned)(gnw_sh2_s3_folds & 0xffffffffu);
    (void)save_r;
}

static void h_snap(SH2 *dst) { *dst = h_sh2; }

static int h_case_event(const char *id, unsigned v, unsigned char byte,
                        int budget, unsigned bop4_dt, unsigned r1)
{
    SH2 o, d, c; unsigned fo, fd, fc; int bad = 0, icd, icc;
    h_reset(v, byte, 0x8411, bop4_dt, r1, v);
    h_run(1, 1, budget, &fo); h_snap(&o); bad |= h_mem_cmp(id, 1);
    h_reset(v, byte, 0x8411, bop4_dt, r1, v);
    h_run(0, 1, budget, &fd); h_snap(&d); bad |= h_mem_cmp(id, 0);
    h_reset(v, byte, 0x8411, bop4_dt, r1, v);
    h_run(0, 0, budget, &fc); h_snap(&c); bad |= h_mem_cmp(id, 0);
    bad |= h_state_cmp(id, &d, &o, 0x7fffffff, &icd);
    bad |= h_state_cmp(id, &c, &o, 0x7fffffff, &icc);
    /* the 0xaffe park drains both arms via the pre-existing BRA-self
     * fastfold (`icount %= 3`), so exact residuals are not comparable;
     * require both drained into the same 0..2 window instead. The loop
     * phase's per-iteration accounting (7 cycles, body 4 + taken BF 3)
     * is identical in fold and oracle and is verified exactly by the
     * analytic budget cases below. */
    if (o.icount != d.icount || o.icount != c.icount
        || o.icount < -3 || o.icount > 2) {
        printf("[%s] FAIL spin drain mismatch o=%d d=%d c=%d\n", id,
               (int)o.icount, (int)d.icount, (int)c.icount);
        bad = 1;
    }
    printf("[case %s] %s v=%u byte=%02x budget=%d | oracle_r2=%08x pc=%08x "
           "t=%d ic=%d | D folded=%u r2=%08x ic=%d | C folded=%u r2=%08x "
           "ic=%d | real_iters_oracle=%u\n",
           id, bad ? "FAIL" : "PASS", v, byte, budget,
           o.r[2], o.pc, o.t_flag, (int)o.icount,
           fd, d.r[2], (int)d.icount, fc, c.r[2], (int)c.icount,
           byte ? 1u : v);
    return bad;
}

/* budget-bounded: no shared event boundary with the oracle possible; check
 * the D arm against the analytic spec (k = min((ic-1)/7, v-1) per matcher
 * dispatch, arithmetic accumulated) and record actual totals. */
/* Analytic fold model (production D semantics), matching the matcher's
 * accounting: a real iteration is MOV/TST/BFnt/DT (4 cycles) + the BF-back
 * dispatch (1). At each BF-back dispatch the matcher sees v = r2 (already
 * DT-decremented) and, when the gate passes, folds k = min((ic-1)/7, v-1)
 * iterations at 7 cycles each, then the real BF-back executes (1 cycle).
 * DT expiry (r2==0) makes the BF-back dispatch fall through to the two
 * exit NOPs and the self-branch spin drains whatever budget is left.
 * Returns final r2 / ic and whether the loop exited via DT. */
static void h_model(unsigned v, int B, unsigned *r2f, int *icf, int *exited)
{
	/* GNW: per-iteration cost is 7 guest cycles = 4 body insns at 1
	 * + taken BF-back at 3 (measured by the per-insn tracer, s3h_run4:
	 * ic 61 -> 54 -> 47 ...). The fold charges exactly the same 7/iter
	 * (k*7) plus the one real BF-back dispatch, so oracle and fold land
	 * on identical state at equal budgets. */
	long long ic = B;
	unsigned r2 = v;
	while (1) {
		if (ic < 5) {	/* stopped mid-iteration, pc inside loop */
			*exited = 0; break; }
		ic -= 7; r2 -= 1;
		if (r2 == 0) {
			/* DT hit zero: BF-back not taken (T=1), two NOPs land, spin */
			*exited = 1; break;
		}
		if (ic <= 0) { *exited = 0; break; } /* ran past the slice */
	}
	*r2f = r2; *icf = (int)ic;
}

static int h_case_budget(const char *id, unsigned v, int budget)
{
	SH2 d, c, o; unsigned fd, fc, fo; int bad = 0, icd, icc;
	unsigned mm_r2; int mm_ic, mm_ex;
#ifdef GNW_S3_TRACE
	extern int gnw_sh2_s3_trace;
	gnw_sh2_s3_trace = (budget <= 100);	/* small budgets only */
#endif
	h_model(v, budget, &mm_r2, &mm_ic, &mm_ex);
	/* All three arms run the SAME guest budget: fold and oracle must
	 * make identical guest progress (same r2, same exit, same residual
	 * icount); only host dispatch work differs. Verified by full state
	 * compare with ic_diff expected 0 — these cases end inside the loop
	 * (no 0xaffe spin drain), so exact residual equality is meaningful. */
	h_reset(v, 0, 0x8411, 0x4210, H_PA - 1, v);
	h_run(1, 1, budget, &fo); h_snap(&o); bad |= h_mem_cmp(id, 1);
	h_reset(v, 0, 0x8411, 0x4210, H_PA - 1, v);
	h_run(0, 1, budget, &fd); h_snap(&d); bad |= h_mem_cmp(id, 0);
	h_reset(v, 0, 0x8411, 0x4210, H_PA - 1, v);
	h_run(0, 0, budget, &fc); h_snap(&c); bad |= h_mem_cmp(id, 0);
	bad |= h_state_cmp(id, &d, &o, 0, &icd);
	bad |= h_state_cmp(id, &c, &o, 0, &icc);
	if (d.r[2] != mm_r2) {
		printf("[%s] FAIL r2 %08x != model %08x\n", id, d.r[2], mm_r2);
		bad = 1;
	}
	{
		int dexited = (d.pc == H_L + 14);
		if (dexited != mm_ex) {
			printf("[%s] FAIL exited %d != model %d (pc=%08x)\n",
				id, dexited, mm_ex, d.pc);
			bad = 1;
		}
	}
	printf("[case %s] %s v=%08x budget=%d | model r2=%08x ic=%d ex=%d | "
	       "D r2=%08x ic=%d pc=%08x folded=%u | C folded=%u | "
	       "O r2=%08x ic=%d pc=%08x folded=%u (O==D==C full state, "
	       "ic residual diff 0)\n",
	       id, bad ? "FAIL" : "PASS", v, budget, mm_r2, mm_ic, mm_ex,
	       d.r[2], (int)d.icount, d.pc, fd, fc,
	       o.r[2], (int)o.icount, o.pc, fo);
	return bad;
}

/* Positive m != disp address-decode case (team-lead directive): the
 * default 0x8411 has m == disp == 1, so the original s3 operand-swap bug
 * (r[d] + m instead of r[m] + d) was undetectable there. 0x8421 encodes
 * m=2 (base R2), d=1 -> correct ea = r[2]+1; the buggy decode sampled
 * r[1]+2. Arrange the correct ea to read 0 (folds) and the buggy ea to
 * read 0xff (would refuse to fold): fold>0 in D and C proves the decode
 * uses r[m]+d. The mirrored variant (0xff at the correct ea, 0 at the
 * buggy ea) must produce folded==0 (fb!=0 gate). dt=R3 (base!=dt!=0). */
static int h_case_mdisp(int positive, unsigned v)
{
	const unsigned bop1 = 0x8421;	/* MOV.B @(1,R2),R0 */
	const unsigned bop4 = 0x4310;	/* DT R3 */
	/* correct ea = r[2]+1 = H_PA ; buggy ea = r[1]+2 = W_PA */
	const unsigned W_PA = 0x06000222u;
	unsigned char ok_byte = positive ? 0x00 : 0xff;
	unsigned char bad_byte = positive ? 0xff : 0x00;
	SH2 o, d, c; unsigned fo, fd, fc; int bad = 0, icd, icc;
	char id[32];
	snprintf(id, sizeof id, "mdisp_%s", positive ? "pos" : "neg");
	/* r[2]=H_PA-1 (base, m=2), r[1]=W_PA-2 (aliased junk), r[3]=v (dt) */
	h_reset(v, ok_byte, bop1, bop4, W_PA - 2, H_PA - 1);
	h_sd[(W_PA & 0x3ffffu) ^ 1u] = bad_byte;
	h_sh2.r[3] = v;
	h_run(1, 1, 10000, &fo); h_snap(&o); bad |= h_mem_cmp(id, 1);
	h_reset(v, ok_byte, bop1, bop4, W_PA - 2, H_PA - 1);
	h_sd[(W_PA & 0x3ffffu) ^ 1u] = bad_byte;
	h_sh2.r[3] = v;
	h_run(0, 1, 10000, &fd); h_snap(&d); bad |= h_mem_cmp(id, 0);
	h_reset(v, ok_byte, bop1, bop4, W_PA - 2, H_PA - 1);
	h_sd[(W_PA & 0x3ffffu) ^ 1u] = bad_byte;
	h_sh2.r[3] = v;
	h_run(0, 0, 10000, &fc); h_snap(&c); bad |= h_mem_cmp(id, 0);
	bad |= h_state_cmp(id, &d, &o, 0x7fffffff, &icd);
	bad |= h_state_cmp(id, &c, &o, 0x7fffffff, &icc);
	if (positive) {
		/* fold must actually happen under the correct decode */
		if (fd == 0 || fc == 0) {
			printf("[%s] FAIL expected positive folds, D=%u C=%u\n",
				id, fd, fc);
			bad = 1;
		}
	} else {
		/* correct ea reads 0xff -> fb!=0 gate refuses in every arm */
		if (fd != 0 || fc != 0 || fo != 0) {
			printf("[%s] FAIL expected no folds, D=%u C=%u O=%u\n",
				id, fd, fc, fo);
			bad = 1;
		}
	}
	/* no stray writes: both probe bytes keep their programmed values */
	if (h_sd[(H_PA & 0x3ffffu) ^ 1u] != ok_byte
	    || h_sd[(W_PA & 0x3ffffu) ^ 1u] != bad_byte) {
		printf("[%s] FAIL probe bytes disturbed\n", id);
		bad = 1;
	}
	printf("[case %s] %s v=%u | D folded=%u r3=%08x ic=%d | C folded=%u | "
	       "O r3=%08x ic=%d | ok_ea=%02x@%08x bug_ea=%02x@%08x\n",
	       id, bad ? "FAIL" : "PASS", v, fd, d.r[3], (int)d.icount, fc,
	       o.r[3], (int)o.icount, ok_byte, H_PA, bad_byte, W_PA);
	return bad;
}

static int rig_s3_harness(void)
{
    int bad = 0;
    /* the harness returns before emu_32x_startup, which is what normally
     * turns the fastloop machinery on — without this the D/C arms never
     * reach the matcher and every comparison is vacuous. */
    gnw_sh2_fastloops = 1;
    printf("[s3h] shape3 differential harness: oracle vs D(prod) vs C(volatile), fastloops=%d\n", gnw_sh2_fastloops);
    /* 1 DT exhaustion, small v — shared event boundary, full compare */
    bad |= h_case_event("dt_v100", 100, 0, 10000, 0x4210, H_PA - 1);
    /* 2 DT exhaustion at the AB-boot scale */
    bad |= h_case_event("dt_v65536", 0x10000, 0, 500000, 0x4210, H_PA - 1);
    /* 3 immediate byte exit: gate must not fold */
    bad |= h_case_event("byte_exit", 100, 0xaa, 10000, 0x4210, H_PA - 1);
    /* 4 alias rejection: DT R1 == base R1 -> matcher must refuse */
    bad |= h_case_event("alias_dt_base", 100, 0, 10000, 0x4110, H_PA - 1);
    /* 5 v==2: exactly one foldable iteration */
    bad |= h_case_event("dt_v2", 2, 0, 1000, 0x4210, H_PA - 1);
    /* 6 v==1: v>=2 gate fails -> identical to oracle, zero folds */
    bad |= h_case_event("dt_v1_nofold", 1, 0, 1000, 0x4210, H_PA - 1);
    /* 7 tiny budget: k=0, no fold, arms identical, stop mid-loop */
    bad |= h_case_event("slice_B6", 100, 0, 6, 0x4210, H_PA - 1);
    /* 8 budget-bounded analytic: v=10, B=61 — model expects r2=1
     * (1 real iteration + 8 folded at dispatch 1) */
    bad |= h_case_budget("slice_B61", 10, 61);
    /* 9 unsigned-large v: the old (int)(v-1) bug would skip the fold
     * entirely (k clamped negative -> 0) and the model would then see
     * pure-interpreter progress — the mismatch exposes the regression. */
    bad |= h_case_budget("unsigned_v80000001", 0x80000001u, 1400);
    /* 10/11 m != disp positive address-decode cases (0x8421): the
     * default 0x8411 (m==disp==1) cannot distinguish r[m]+d from r[d]+m;
     * these pin the sampled address by byte placement in both polarities */
    bad |= h_case_mdisp(1, 100);
    bad |= h_case_mdisp(0, 100);
    printf("[s3h] %s\n", bad ? "HARNESS FAIL" : "HARNESS PASS");
    return bad;
}
#endif /* RIG_S3_HARNESS */

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    uint32_t rom_len = (uint32_t)(_binary_rom_32x_end - _binary_rom_32x_start);

    rig_timer_init();
    uint32_t cal = rig_calibrate(1000000);
    uint32_t ipt_x1000 = (uint32_t)((3000000ull * 1000ull) / (cal ? cal : 1));
    printf("[32x-qemu] cal: 3.0M insns = %lu ticks -> %lu.%03lu insn/tick\n",
           (unsigned long)cal, (unsigned long)(ipt_x1000/1000), (unsigned long)(ipt_x1000%1000));
    printf("[32x-qemu] rom len=%lu (pre-byteswapped) frames=%d\n",
           (unsigned long)rom_len, RIG_FRAMES);
#ifdef RIG_S3_HARNESS
    return rig_s3_harness();
#endif

    /* ---- libretro (upstream frontend) init order — NOT the old device order.
     * 32X startup is LAZY: the game's own MD-mode boot code writes ADEN at
     * 0xA15101 and PicoWrite8_32x calls Pico32xStartup (which itself runs
     * Pico32xPrepare + emu_32x_startup). Calling Pico32xStartup up front, the
     * old device/rig order, pre-enables the adapter and breaks the boot
     * handshake: VF's 68K parks in a nop/bra idle loop at 0x88088e forever.
     * PicoReset is not called either — PicoLoadMedia -> PicoCartInsert ->
     * PicoPower already reset the machine. */
    {
    /* sh2s became a pointer (DTCM on device); the rig does not link
     * main_md32x.c, so give it static storage here. */
    static SH2 sh2s_storage[2];
    sh2s = sh2s_storage;
    memset(sh2s_storage, 0, sizeof sh2s_storage);
  }
  PicoInit();
#ifdef RIG_32X_BIOS
    /* Set before PicoLoadMedia: get_bios() runs inside the load path. */
    p32x_bios_g = (void *)_binary_bios_m68k_start;
    p32x_bios_m = (void *)_binary_bios_msh2_start;
    p32x_bios_s = (void *)_binary_bios_ssh2_start;
    printf("[32x-qemu] real BIOS: g=%u m=%u s=%u bytes\n",
           (unsigned)(_binary_bios_m68k_end - _binary_bios_m68k_start),
           (unsigned)(_binary_bios_msh2_end - _binary_bios_msh2_start),
           (unsigned)(_binary_bios_ssh2_end - _binary_bios_ssh2_start));
#endif
    PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_32X | POPT_EN_PWM
               | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;   /* mono: no EN_STEREO */
    /* Sound-subsystem ablation. The phase table charges Doom's frame for
     * ym2612 (3.2%), z80 (2.0%) and psg/dac/mix (1.0%) whether or not the game
     * uses any of them -- a 32X title can drive its audio entirely from the
     * SH-2 PWM path and leave the MD sound hardware idle. These knobs price
     * each one, and the `snd hash` says whether the game noticed: an unchanged
     * hash with the chip switched off means it was producing silence at cost.
     *   RIG_NO_FM / RIG_NO_Z80 / RIG_NO_PSG */
#ifdef RIG_NO_FM
    PicoIn.opt &= ~POPT_EN_FM;
#endif
#ifdef RIG_NO_Z80
    PicoIn.opt &= ~POPT_EN_Z80;
#endif
#ifdef RIG_NO_PSG
    PicoIn.opt &= ~POPT_EN_PSG;
#endif
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184;   /* US, EU, JP */

    enum media_type_e mt = PicoLoadMedia("game.32x", _binary_rom_32x_start, rom_len,
                                         NULL, NULL, NULL, NULL);
    printf("[32x-qemu] PicoLoadMedia -> media_type=%d AHW=%x\n", (int)mt, (unsigned)PicoIn.AHW);
    {
      /* A cart over 4 MiB is only playable if the SSF2 mapper actually
       * installed. Say so out loud: a silent no-mapper run looks exactly like
       * a wedge, and that ambiguity is what made ">4 MiB" a closed row for
       * months. */
      extern int carthw_ssf2_active;
      printf("[32x-qemu] romlen=%u ssf2_active=%d\n",
             (unsigned)rom_len, carthw_ssf2_active);
    }
#ifdef RIG_32X_SSF
    /* The SSF (bank-switch) mapper, which the firmware compiles out for this
     * core (pico/cart.c's `#ifndef GNW_32X_CORE`, because the zero-copy flash
     * path cannot bank). A >4 MiB cart needs it: the 32X ROM window IS 4 MiB.
     * Rig-only, so an UNMODIFIED 5 MiB D32XR release can be tried without the
     * two things I did to it -- the wad surgery and the self-build. */
    if (rom_len > 0x400000) {
        extern void carthw_ssf2_startup(void);
        extern void (*PicoCartMemSetup)(void);
        carthw_ssf2_startup();
        if (PicoCartMemSetup != NULL) PicoCartMemSetup();
        printf("[32x-qemu] SSF mapper armed for %u byte cart\n", (unsigned)rom_len);
    }
#endif
    if (mt == PM_ERROR) { printf("[32x-qemu] LOAD FAILED\n"); return 3; }

#ifdef RIG_SRAM_FILL
    static int s_sv_dump_hex;
    /* Second-boot replay knobs are COMPILE-TIME (EXTRA_DEF): the guest runs
     * under QEMU semihosting where getenv() cannot see host env vars (the
     * env-var version silently never fired — proven by the missing
     * "[sram] filled" line), but fopen/open are semihosted and do work.
     *   -DRIG_SRAM_PRELOAD_FILE="\"/abs/path\""  fill cart SRAM before frame 0
     *       (firmware equivalent: md32x_SramLoad reading the .sram card file)
     *   -DRIG_SRAM_FILL_BYTE=0xff|0x100          pre-fill (0x100 = random s1234)
     *   -DRIG_SRAM_DUMP_FILE="\"/abs/path\""     write SRAM back at exit so the
     *       next run can be fed exactly what this run's game wrote. */
    {
        extern void *rig_sram_ptr(u32 *size);
        u32 sv_size = 0;
        unsigned char *sv = rig_sram_ptr(&sv_size);
        printf("[sram] size=%u ptr=%p\n", sv_size, (void *)sv);
#ifdef RIG_SRAM_PRELOAD_HEADER
        if (sv) {
            u32 n = rig_preload_bin_len < sv_size ? rig_preload_bin_len : sv_size;
            memcpy(sv, rig_preload_bin, n);
            printf("[sram] preloaded %u bytes from linked header\n", n);
        }
#endif
#ifdef RIG_SRAM_FILL_BYTE
        if (sv) {
            unsigned v = RIG_SRAM_FILL_BYTE;
            srandom(1234);
            for (u32 i = 0; i < sv_size; i++)
                sv[i] = (v == 0x100) ? (random() & 0xff) : (unsigned char)v;
            printf("[sram] filled 0x%x bytes with %s\n", sv_size,
                   v == 0x100 ? "random(seed 1234)" : "constant");
        }
#endif
#ifdef RIG_SRAM_DUMP_HEX
        s_sv_dump_hex = 1;
#endif
    }
#endif

#ifdef RIG_IDLE_SKIP
    {
        extern int gnw_sh2_idle_skip;
        gnw_sh2_idle_skip = 1;
        printf("[32x-qemu] idle_skip ENABLED (compile-time)\n");
    }
#endif

    PicoLoopPrepare();
    PicoIn.sndOut = s_snd;
    PicoIn.writeSound = rig_write_sound;
    PsndRerate(0);
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    set_out_buffer();

#ifdef RIG_STATE_TEST
    if (state_roundtrip_test() != 0) return 5;
#endif

    uint64_t tot = 0, mn = ~0ull, mx = 0, sh2_tot = 0;
    uint64_t tot_drawn = 0, tot_skip = 0;
    uint32_t n_drawn = 0, n_skip = 0;
    uint64_t tot_w1 = 0, tot_w2 = 0;   /* soak drift windows (first/last 500) */
    unsigned long long sh2_prev = g_sh2_insns;
    uint32_t cks100 = 0, cks300 = 0, cksend = 0;
    int nb100 = 0, nb300 = 0, nbend = 0;

    for (int f = 0; f < RIG_FRAMES; f++) {
        rig_frame_no = f;
#ifdef RIG_STATE_LOAD
        /* Resume the device's scene. Everything before this frame is the cold
         * boot needed to bring the adapter up, so keep RIG_STATE_LOAD_AT at or
         * below RIG_WARMUP -- otherwise the measurement window averages boot
         * frames in with gameplay ones and reports neither. */
        if (f == RIG_STATE_LOAD_AT && rig_load_device_state() != 0) {
            printf("[32x-qemu] ABORT: device savestate did not load\n");
            return 6;
        }
#endif
#ifdef RIG_STRPAGE
        if (f == 8 && !s_strp_installed) { s_strp_installed = 1; rig_strp_install(); }
#endif
        if (f == RIG_WARMUP) phase_snapshot();
#ifdef RIG_Z80_HIST
        if (f == RIG_Z80_HIST_FROM) {
            extern unsigned long long g_z80_pc_hist[0x10000];
            extern unsigned long long g_z80_insns;
            memset(g_z80_pc_hist, 0, sizeof(g_z80_pc_hist));
            g_z80_insns = 0;
            printf("[32x-z80pc] histogram zeroed at frame %d\n", f);
        }
#endif
#ifdef RIG_HIST_FROM
        /* GLM debug: zero the SH-2 PC histogram tables here so the final
         * report covers only frames >= RIG_HIST_FROM (e.g. the post-death
         * window of the D32XR stop) instead of the whole run. */
        if (f == RIG_HIST_FROM) {
            extern struct rig_pc_slot rig_pchist[2][RIG_PC_HIST_SLOTS];
            memset(rig_pchist, 0, sizeof(rig_pchist));
            printf("[glm-hist] histogram zeroed at frame %d\n", f);
        }
#endif
#ifdef RIG_MEM_MIX
        /* Zero the access census at gameplay entry. Without this the ~400
         * title/menu frames the pad script walks through outnumber the
         * gameplay frames 80:1 and the census describes the menu. */
        if (f == RIG_MIX_FROM) {
            extern unsigned long long gnw_census[6][5];
            extern unsigned long long gnw_census_pagehit[];
            extern unsigned int gnw_census_page[];
            extern unsigned long long gnw_census_page_missed;
            extern int gnw_census_npages;
            memset(gnw_census, 0, sizeof(gnw_census));
            memset(gnw_census_pagehit, 0, sizeof(unsigned long long) * 512);
            memset(gnw_census_page, 0, sizeof(unsigned int) * 512);
            gnw_census_page_missed = 0;
            gnw_census_npages = 0;
            printf("[32x-mix] census reset at frame %d (gameplay entry)\n", f);
        }
#endif
#ifdef RIG_SECOND_BOOT
        /* pico_int.h is deliberately not included (PicoDrawSetOutputSMS
         * collision), so reach the two structs through char-base offsets:
         * struct Pico32x { u16 regs[0x20]; u16 vdp_regs[0x10]; ... }
         * -> vdp_regs[0] is at byte offset 0x40. */
        extern char Pico, Pico32x;
#define RIG_VDP0() (*(unsigned short *)(&Pico32x + 0x40))
        if (f == RIG_SECOND_BOOT_AT) {
            /* GNW: reproduce the device reboot re-entry. On the device a
             * hardware reset restarts the firmware which re-runs
             * PicoInit -> PicoLoadMedia -> (PicoCartInsert -> PicoPower),
             * while NOLOAD overlays / DTCM / AHB keep their previous
             * contents. Here we keep everything allocated (sh2s_storage,
             * Pico32xMem, ahb heap) and only re-run the software init
             * chain, then watch whether the 2nd boot reaches
             * vdp_regs[0] mode!=0 (display on) like the 1st boot did. */
            printf("[2nd] === reboot re-entry at f=%d (vdp0=%04x) ===\n",
                   f, RIG_VDP0());
            /* memset of Pico happens via PicoInit() below (it zeroes the
             * struct itself); sh2s_storage intentionally NOT cleared. */
            PicoInit();
            PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
                       | POPT_EN_32X | POPT_EN_PWM
                       | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;
            PicoIn.sndRate = 44100;
            PicoIn.autoRgnOrder = 0x184;
            enum media_type_e mt2 = PicoLoadMedia("game.32x",
                    _binary_rom_32x_start, rom_len, NULL, NULL, NULL, NULL);
            printf("[2nd] PicoLoadMedia -> mt=%d AHW=%x vdp0=%04x\n",
                   (int)mt2, (unsigned)PicoIn.AHW, RIG_VDP0());
        }
        if (f >= RIG_SECOND_BOOT_AT && (f % 50) == 0) {
            int nblk = 0; uint32_t ck = fb_checksum(&nblk);
            printf("[2nd] f=%d vdp0=%04x fb_ck=%08x nb=%d\n",
                   f, RIG_VDP0(), ck, nblk);
        }
#undef RIG_VDP0
#endif
        int skipf = skip_this_frame(f);
        PicoIn.skipFrame = (unsigned short)skipf;
        PicoIn.pad[0] = pad_script(f);
        rig_frame_idx = f;   /* GNW: expose frame index to sh2pico probes */
        uint32_t t0 = rig_timer_now();
        PicoFrame();
        uint32_t t1 = rig_timer_now();
#ifdef RIG_YM_CENSUS
        if (f >= RIG_YM_CENSUS_FROM) ym_census_frame();
#endif
        uint64_t insn = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000;

        unsigned long long sh2_now = g_sh2_insns, sh2_d = sh2_now - sh2_prev;
        sh2_prev = sh2_now;

#ifdef RIG_TRACE_COST
        /* Every frame, not every twentieth. The gameplay window's host cost
         * ran 8.0M to 3.74G insn/frame -- a 460x spread whose mean is not a
         * speed -- and a 20-frame stride cannot tell a periodic spike from a
         * one-off catch-up. Prints guest insns beside host insns because the
         * interesting frames are the ones where host work climbs while guest
         * work does not: that is the scheduler, not the renderer. */
        if (f >= RIG_TRACE_COST)
            printf("[cost] f=%d host=%llu sh2=%llu\n", f,
                   (unsigned long long)insn, sh2_d);
#endif

#ifdef RIG_SH2_WATCH
        /* D32XR hunt: bracket the frame where the guest SH-2s leave game
         * code for the BIOS re-entry. First watch showed both CPUs already
         * inside the BIOS checksum loop at f=160, so the exit happened
         * earlier: this edge detector logs the SDRAM->BIOS transition frame
         * itself. pc@0x40 ppc@0x44, sizeof(SH2)=6016. */
        if (f >= 4 && f < 160) {
            static unsigned prev_m, prev_s;
            unsigned char *m = (unsigned char *)sh2s, *s = (unsigned char *)sh2s + 6016;
            unsigned cm = *(unsigned *)(m + 0x40), cs = *(unsigned *)(s + 0x40);
            if ((prev_m >= 0x02000000 && cm < 0x001000) ||
                (prev_m >= 0x06000000 && cm < 0x001000))
                printf("[sh2w] f=%d M->BIOS pc=%08x prev=%08x ppc=%08x\n",
                       f, cm, prev_m, *(unsigned *)(m + 0x44));
            if ((prev_s >= 0x02000000 && cs < 0x001000) ||
                (prev_s >= 0x06000000 && cs < 0x001000))
                printf("[sh2w] f=%d S->BIOS pc=%08x prev=%08x ppc=%08x\n",
                       f, cs, prev_s, *(unsigned *)(s + 0x44));
            prev_m = cm; prev_s = cs;
        }
        /* companion: the SH-2s parked at f=176 waiting for the 68K to answer
         * on comm0 (0x560 loop), so the real question is where the 68K is.
         * Every 10th frame early, every frame around the death. */
        if (f % 10 == 0 || f >= 165)
            printf("[m68w] f=%d pc=%08x\n", f,
                   (unsigned)(m68k.pc & 0xffffff));
#endif

#ifdef RIG_SDRAM_SCAN
        /* GLM: D32XR Z_Malloc hunt. Once the SH-2s die (sh2_d==0 after the
         * game ran), find every "Z_Malloc" copy in SDRAM — in plain and in
         * pair-swapped byte order, the buffer may hold either — and dump
         * context around it, so the ROM-window reader hook can be re-aimed
         * at the relocated string. One shot. */
        if (f >= 150 && sh2_d == 0 && !s_zscan_done) {
            extern struct Pico32xMem *Pico32xMem;
            static const unsigned char pat[2][8] = {
                { 'Z','_','M','a','l','l','o','c' },          /* plain     */
                { '_','Z','a','M','l','l','c','o' },          /* pair-swapped */
            };
            s_zscan_done = 1;
            printf("[zscan] at f=%d m68k.pc=%08x\n", f,
                   (unsigned)(m68k.pc & 0xffffff));
            /* the rebooted program died at SDRAM 0x554..0x562 (first watch);
             * dump that window so it can be disassembled */
            if (Pico32xMem) {
                const unsigned char *sd = (const unsigned char *)Pico32xMem;
                printf("[zscan] sdram+0x540:");
                for (unsigned i = 0x540; i < 0x5e0; i += 2) {
                    unsigned gw = sd[i] | (sd[i + 1] << 8);
                    printf(" %04x", gw);
                    if ((i & 0x1e) == 0x1e) printf("\n[zscan] sdram+%03x:", i + 2);
                }
                printf("\n");
            }
            if (Pico32xMem) {
                const unsigned char *sd = (const unsigned char *)Pico32xMem;
                for (int p = 0; p < 2; p++)
                    for (unsigned i = 0; i < 0x40000 - 8; i++)
                        if (sd[i] == pat[p][0] && !memcmp(sd + i, pat[p], 8)) {
                            printf("[zscan] %s copy at sdram+%05x "
                                   "(guest %08x):", p ? "swapped" : "plain",
                                   i, 0x06000000 + i);
                            for (int j = -8; j < 56; j++)
                                printf(" %02x", sd[i + j]);
                            printf("\n");
             }
         }
#endif

#ifdef RIG_DEATH_STACK
        /* companion to the header block: fire once at the chosen frame and
         * walk the frozen SH-2 state (see header comment for offsets). */
        /* pre-death per-frame tracking: the T_START lookup returned 0x6161
         * while the death-time count/table say 2079/0x0202d00c, which the
         * walk cannot produce -- so one of those variables held something
         * else when the lookup ran. Watch all three every frame leading
         * into the death frame (guest u32 = halfword-swapped host u32). */
        if (f >= RIG_DEATH_STACK - 100 && f <= RIG_DEATH_STACK) {
            extern unsigned char carthw_ssf2_banks[8];
            extern const unsigned char _binary_rom_32x_start[];
            const unsigned char *sd = (const unsigned char *)Pico32xMem;
            unsigned raw_c = *(const unsigned *)(sd + 0x3ad9c);
            unsigned raw_t = *(const unsigned *)(sd + 0x8164);
            unsigned raw_s = *(const unsigned *)(sd + 0x8222);
            /* T_START match candidate the walk's 0x6161 index implies:
             * r0 = table + 0x6161*16 = 0x0208e61c, name u32s at +8/+12.
             * Resolve them through the live SSF2 bank mapping the way
             * sh2_read32_rom does, so a banked T_START copy is visible. */
            unsigned bank = carthw_ssf2_banks[4] << 19;
            const unsigned *rom32 = (const unsigned *)_binary_rom_32x_start;
            unsigned n0 = __builtin_bswap32(
                rom32[(bank + (0x0208e624 & 0x7fffc)) / 4]);
            unsigned n1 = __builtin_bswap32(
                rom32[(bank + (0x0208e628 & 0x7fffc)) / 4]);
            printf("[trk] f=%d count=%08x tbl=%08x slot=%08x bk=%02x%02x%02x%02x%02x%02x%02x%02x cand=%08x %08x\n", f,
                   ((raw_c & 0xffff) << 16) | (raw_c >> 16),
                   ((raw_t & 0xffff) << 16) | (raw_t >> 16),
                   ((raw_s & 0xffff) << 16) | (raw_s >> 16),
                   carthw_ssf2_banks[0], carthw_ssf2_banks[1],
                   carthw_ssf2_banks[2], carthw_ssf2_banks[3],
                   carthw_ssf2_banks[4], carthw_ssf2_banks[5],
                   carthw_ssf2_banks[6], carthw_ssf2_banks[7],
                   n0, n1);
        }
        if (f == RIG_DEATH_STACK && !s_ds_done) {
            const unsigned char *sd = (const unsigned char *)Pico32xMem;
            s_ds_done = 1;
            for (int c = 0; c < 2; c++) {
                const unsigned char *cpu = (unsigned char *)sh2s + c * 6016;
                printf("[ds] f=%d cpu=%c\n", f, c ? 's' : 'm');
                for (unsigned o = 0; o <= 0x58; o += 4)
                    printf("[ds]  +%02x %08x\n", o,
                           *(const unsigned *)(cpu + o));
            }
            /* WAD bookkeeping window: D32XR keeps its wad/lumpinfo state in
             * SDRAM around 0x0603ad70-0x0603ada0 (literal pool of WAD-init,
             * 5MiB ROM 0x201a60c-0x201a634). Dump it plus whatever the
             * lumpinfo table pointer at +0x2c leads to, so a garbage parse
             * is visible without guessing which field is which. */
            if (sd) {
                for (int i = 0; i < 0x90; i += 4)
                    printf("[ds] wad %08x %08x\n", 0x0603ad70 + i,
                           *(const unsigned *)(sd + 0x3ad70 + i));
                unsigned lit = *(const unsigned *)(sd + 0x3ad9c);
                if ((lit >> 24) == 0x06 && !(lit & 1)) {
                    unsigned lo = lit & 0x3fffc;
                    for (int i = 0; i < 0x100; i += 4)
                        printf("[ds] lit %08x %08x\n", 0x06000000 + lo + i,
                               *(const unsigned *)(sd + lo + i));
                }
                /* lumpinfo table pointer variable (ROM lit 0x201686c ->
                 * 0x06008164 in the 5MiB build); walk it like
                 * W_CheckNumForName does and show the head of the table plus
                 * the ROM directory's T_START slot (index 242). */
                /* count slot the T_START lookup stores into (0x06008222)
                 * plus its neighbourhood, to see the u16 the walk returned. */
                for (int i = 0; i < 0x40; i += 4)
                    printf("[ds] cnt %08x %08x\n", 0x06008220 + i,
                           *(const unsigned *)(sd + 0x8220 + i));
                /* lump-data cache functions live in SDRAM game code
                 * (0x06005140 = cache(idx)->ptr per the death stack; its
                 * neighbours 0x06005110 print, 0x06006bd0/0x06006c00 are
                 * called by WAD-init). Disassembly source for the pointer
                 * arithmetic that produced 0x2233BA68. */
                for (int i = 0; i < 0x400; i += 4)
                    printf("[ds] cache %08x %08x\n", 0x06005100 + i,
                           *(const unsigned *)(sd + 0x5100 + i));
                /* cache2 window widened to 0x06006a00-0x06006e00: the
                 * opcode-0x01 proxy-read requesters observed writing
                 * comm1=0x070N at ppc=0x06006b4c/0x06006b4e live just below
                 * the 0x06006bd0 bank callback (lm5.log). Need their disasm
                 * to learn what the 0x0708 index encodes. */
                for (int i = 0; i < 0x400; i += 4)
                    printf("[ds] cache2 %08x %08x\n", 0x06006a00 + i,
                           *(const unsigned *)(sd + 0x6a00 + i));
                unsigned tbl = *(const unsigned *)(sd + 0x8164);
                printf("[ds] lumpinfo_ptr=%08x\n", tbl);
                if ((tbl >> 24) == 0x06 && !(tbl & 1)) {
                    unsigned to = tbl & 0x3fffc;
                    for (int i = 0; i < 0x100; i += 4)
                        printf("[ds] tbl %08x %08x\n", tbl + i,
                               *(const unsigned *)(sd + to + i));
                    unsigned ti = to + 242 * 16;
                    if (ti + 16 <= 0x40000)
                        for (int i = 0; i < 0x40; i += 4)
                            printf("[ds] t242 %08x %08x\n", tbl + 242*16 + i,
                                   *(const unsigned *)(sd + ti - 0x20 + i));
                }
            }
            unsigned r15 = *(const unsigned *)((unsigned char *)sh2s + 0x3c);
            printf("[ds] msh2 r15=%08x\n", r15);
            if (sd && (r15 >> 24) == 0x06) {
                unsigned base = r15 & 0x3fffc;
                for (int i = 0; i < 320; i++) {
                    unsigned a = base + i * 4;
                    if (a + 4 > 0x40000) break;
                    unsigned v = *(const unsigned *)(sd + a);
                    printf("[ds]  stk %08x %08x\n", 0x06000000 + a, v);
                }
                for (int i = 0; i < 320; i++) {          /* SDRAM code windows */
                    unsigned a = base + i * 4;
                    if (a + 4 > 0x40000) break;
                    unsigned v = *(const unsigned *)(sd + a);
                    if ((v >> 24) != 0x06 || (v & 1)) continue;
                    unsigned lo = (v & 0x3fffc) - 0x20;
                    if (lo > 0x40000 - 0x60) continue;
                    printf("[ds]  win %08x:", v);
                    for (unsigned j = 0; j < 48; j++)
                        printf(" %04x", *(const unsigned short *)(sd + lo + j * 2));
                     printf("\n");
                 }
             }
            {
                /* live 68K RAM: the boot block copies game code from ROM
                 * (file 0x5a30+) into 0xff0000 here, and the RIG_LM_TRACE
                 * comm-write pcs match this copy, not the ROM bytes. */
                extern void *rig_pico_ram(int *len);
                int mlen = 0;
                const unsigned char *mram = rig_pico_ram(&mlen);
                if (mram != NULL && mlen > 0) {
                    for (int i = 0; i + 16 <= mlen; i += 16)
                        printf("[mram %04x] %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                               i, mram[i],mram[i+1],mram[i+2],mram[i+3],
                               mram[i+4],mram[i+5],mram[i+6],mram[i+7],
                               mram[i+8],mram[i+9],mram[i+10],mram[i+11],
                               mram[i+12],mram[i+13],mram[i+14],mram[i+15]);
                }
            }
        }
#endif

        /* GNW: window gate for the SH-2 PC/backedge/block census — every
         * PC/op/block statistic must come from ONE fixed scene window
         * (team-lead correction: boot-inclusive figures are not combat
         * ratios). Defaults keep the legacy full-run behavior. */
#ifdef RIG_SH2_PC_HIST
#ifndef RIG_CENSUS_START
#define RIG_CENSUS_START 0
#endif
#ifndef RIG_CENSUS_END
#define RIG_CENSUS_END 0x7fffffff
#endif
        rig_sh2_census_on = (f >= RIG_CENSUS_START && f < RIG_CENSUS_END);
#endif
        if ((f % 20) == 0 || (f >= 140 && f < 260)) {
#ifdef GNW_SH2_DRC
            { /* GNW: counters live in sh2pico.c under -DGNW_SH2_DRC */
              extern unsigned long long gnw_sh2_drc_lookups, gnw_sh2_drc_misses,
                gnw_sh2_drc_runs, gnw_sh2_drc_trans, gnw_sh2_drc_flushes;
            printf("  f%04d host=%lu sh2=%llu snd=%u/%u look=%llu miss=%llu runs=%llu tr=%llu fl=%llu\n", f,
                   (unsigned long)insn, sh2_d, s_snd_calls, s_snd_samples,
                   gnw_sh2_drc_lookups, gnw_sh2_drc_misses, gnw_sh2_drc_runs,
                   gnw_sh2_drc_trans, gnw_sh2_drc_flushes);
            }
#else
            /* GNW: DRC counters only exist under -DGNW_SH2_DRC; true-clean
             * builds previously linked a STALE elf because this line broke
             * them (2026-09-26 speed-bomb hunt). */
            printf("  f%04d host=%lu sh2=%llu snd=%u/%u\n", f,
                   (unsigned long)insn, sh2_d, s_snd_calls, s_snd_samples);
#endif
        }
        if (f >= RIG_WARMUP) {
            tot += insn; sh2_tot += sh2_d;
            if (insn < mn) mn = insn;
            if (insn > mx) mx = insn;
            if (skipf) { tot_skip += insn; n_skip++;
#ifdef RIG_FRAME_HIST
                         if (s_fh_n_skip < RIG_FRAMES) s_fh_skip[s_fh_n_skip++] = (uint32_t)insn;
#endif
                       }
            else       { tot_drawn += insn; n_drawn++;
#ifdef RIG_FRAME_HIST
                         if (s_fh_n_drawn < RIG_FRAMES) s_fh_drawn[s_fh_n_drawn++] = (uint32_t)insn;
#endif
                       }
            if (f < RIG_WARMUP + 500)   tot_w1 += insn;
            if (f >= RIG_FRAMES - 500)  tot_w2 += insn;
        }
#ifdef RIG_FB_DUMP
        if (f == RIG_FB_DUMP) fb_dump_rle(f);
#endif
#ifdef RIG_FB_DUMP2
        if (f == RIG_FB_DUMP2) fb_dump_rle(f);
#endif
#ifdef RIG_FB_DUMP3
        if (f == RIG_FB_DUMP3) fb_dump_rle(f);
#endif
#ifdef RIG_TRACE_CKS
        { int nbt; uint32_t ck = fb_checksum(&nbt);
          printf("cks f%04d=%08lx nb=%d\n", f, (unsigned long)ck, nbt); }
#endif
#ifdef RIG_TRACE_PC
        { unsigned sp = m68k.dar[15] & 0x00ffffff;
          printf("pc  f%04d=%06lx im=%x sp=%06x stk=", f,
                 (unsigned long)(m68k.pc & 0x00ffffff),
                 (unsigned)(m68k.int_mask >> 8), sp);
          for (int k = 0; k < 8; k++) printf("%04x ", rig_m68k_peek16(sp + 2u * k));
          printf("\n"); }
#endif
        if (f == CK_A)    cks100 = fb_checksum(&nb100);
        if (f == CK_B)    cks300 = fb_checksum(&nb300);
        if (f == CK_LAST) cksend = fb_checksum(&nbend);
    }

#ifdef RIG_MEM_MIX
    /* SH-2 data-access census by class and region (pico/32x/memory.c). Opcode
     * fetches are NOT counted -- they take the inline fast path and their
     * count is the dispatched-instruction count anyway. Use this to size a
     * read/write fast path before writing one: a class that is a small share
     * of accesses cannot pay for the compare it adds to every access. */
    {
        extern unsigned long long gnw_census[6][5];
        static const char *const opn[6] = { "r8", "r16", "r32", "w8", "w16", "w32" };
        static const char *const rgn[5] = { "sdram", "rom", "dram", "cs0", "other" };
        unsigned long long grand = 0;
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 5; j++) grand += gnw_census[i][j];
        printf("[32x-mix] SH-2 data accesses, total=%llu\n", grand);
        for (int i = 0; i < 6; i++) {
            unsigned long long row = 0;
            for (int j = 0; j < 5; j++) row += gnw_census[i][j];
            if (!row) continue;
            printf("[32x-mix]  %-4s tot=%-12llu", opn[i], row);
            for (int j = 0; j < 5; j++)
                printf(" %s=%llu(%lu%%)", rgn[j], gnw_census[i][j],
                       (unsigned long)(row ? gnw_census[i][j] * 100 / row : 0));
            printf("  [%lu%% of all]\n",
                   (unsigned long)(grand ? row * 100 / grand : 0));
        }
    }
    {
        /* Cart-ROM READS by 512-byte page, hottest first. This is the whole
         * question behind a RAM cache for XIP flash data: if the top few pages
         * carry most of the reads, a cache measured in kilobytes catches them.
         * Doom's colormap is 256 entries x 16 bit = exactly one page here. */
        extern unsigned long long gnw_census_pagehit[];
        extern unsigned int gnw_census_page[];
        extern unsigned long long gnw_census_page_missed;
        extern int gnw_census_npages;
        int n = gnw_census_npages;
        unsigned long long tot = gnw_census_page_missed;
        for (int i = 0; i < n; i++) tot += gnw_census_pagehit[i];
        printf("[32x-page] %d distinct 512B cart-ROM pages read, %llu reads, "
               "%llu past the table\n", n, tot, gnw_census_page_missed);
        /* selection sort of the top 16 -- n is at most 512 and this runs once */
        for (int k = 0; k < 16 && k < n; k++) {
            int best = k;
            for (int i = k + 1; i < n; i++)
                if (gnw_census_pagehit[i] > gnw_census_pagehit[best]) best = i;
            unsigned long long h = gnw_census_pagehit[best];
            unsigned int pg = gnw_census_page[best];
            gnw_census_pagehit[best] = gnw_census_pagehit[k];
            gnw_census_page[best]    = gnw_census_page[k];
            gnw_census_pagehit[k] = h; gnw_census_page[k] = pg;
        }
        unsigned long long cum = 0;
        for (int k = 0; k < 16 && k < n; k++) {
            cum += gnw_census_pagehit[k];
            printf("[32x-page]  #%-2d addr=%08x reads=%-10llu %5.2f%%  cum %5.2f%%\n",
                   k + 1, gnw_census_page[k] << 9, gnw_census_pagehit[k],
                   tot ? 100.0 * gnw_census_pagehit[k] / tot : 0.0,
                   tot ? 100.0 * cum / tot : 0.0);
        }
        printf("[32x-page] top16 = %llu of %llu reads (%.1f%%) = %d KB of cache\n",
               cum, tot, tot ? 100.0 * cum / tot : 0.0,
               (16 < n ? 16 : n) * 512 / 1024);
    }
#endif

    int n = RIG_FRAMES - RIG_WARMUP;
    printf("[32x-qemu] done %d frames  avg host=%lu  min=%lu  max=%lu insn/frame  avg sh2=%llu\n",
           RIG_FRAMES, (unsigned long)(n > 0 ? tot / n : 0), (unsigned long)mn,
           (unsigned long)mx, (unsigned long long)(n > 0 ? sh2_tot / n : 0));
    if (n_skip > 0 && n_drawn > 0)
        printf("[32x-qemu] skip3: drawn avg=%lu (n=%lu)  skipped avg=%lu (n=%lu)  skip/drawn=%lu%%\n",
               (unsigned long)(tot_drawn / n_drawn), (unsigned long)n_drawn,
               (unsigned long)(tot_skip / n_skip),   (unsigned long)n_skip,
               (unsigned long)(100 * (tot_skip / n_skip) / (tot_drawn / n_drawn)));
    if (RIG_FRAMES >= RIG_WARMUP + 1000)   /* disjoint windows only */
        printf("[32x-qemu] drift: first500 avg=%lu  last500 avg=%lu\n",
                (unsigned long)(tot_w1 / 500), (unsigned long)(tot_w2 / 500));
#ifdef RIG_FRAME_HIST
    fh_report("drawn", s_fh_drawn, s_fh_n_drawn);
    fh_report("skipped", s_fh_skip, s_fh_n_skip);
#endif
#ifdef RIG_ABLATE_LOOPS
    {
        extern unsigned long long rig_abl_hits[2], rig_abl_iters_skipped[2];
        printf("[32x-abl] loop1 entries=%llu iters_skipped=%llu | "
               "loop2 entries=%llu iters_skipped=%llu\n",
               rig_abl_hits[0], rig_abl_iters_skipped[0],
               rig_abl_hits[1], rig_abl_iters_skipped[1]);
        printf("[32x-abl] the screen is wrong on purpose; compare avg host "
               "insn/frame against the same build without -DRIG_ABLATE_LOOPS\n");
    }
#endif
#ifdef RIG_OPCOST
    {
        extern unsigned long long rig_opcost[2][16], rig_opcnt[2][16];
        static const char *nm[16] = {
          "0x0 misc/NOP/RTS","0x1 MOV.L @(d,Rn)","0x2 MOV.x Rm,@Rn","0x3 ALU",
          "0x4 shift/DT/JSR","0x5 MOV.L @(d,Rm)","0x6 MOV.x @Rm,Rn","0x7 ADD #imm",
          "0x8 BT/BF/CMP","0x9 MOV.W @(d,PC)","0xA BRA","0xB BSR",
          "0xC misc/imm","0xD MOV.L @(d,PC)","0xE MOV #imm","0xF (illegal)" };
        for (int c = 0; c < 2; c++) {
            unsigned long long tot = 0;
            for (int i = 0; i < 16; i++) tot += rig_opcost[c][i];
            if (!tot) continue;
            printf("[32x-opcost] %s: ticks by opcode group (ratios only)\n",
                   c ? "ssh2" : "msh2");
            for (int i = 0; i < 16; i++) {
                if (!rig_opcnt[c][i]) continue;
                printf("[32x-opcost]   %-20s %5.2f%%  n=%llu  avg=%.1f\n",
                       nm[i], 100.0 * rig_opcost[c][i] / (double)tot,
                       rig_opcnt[c][i],
                       (double)rig_opcost[c][i] / (double)rig_opcnt[c][i]);
            }
        }
        {
            extern unsigned long long rig_opnow_cost, rig_opnow_n;
            if (rig_opnow_n)
                printf("[32x-opcost] timer span overhead: %.3f ticks (%llu calibrations)\n",
                       (double)rig_opnow_cost / (double)rig_opnow_n,
                       rig_opnow_n);
        }
#ifdef RIG_SH2_SKELETON
        printf("[32x-opcost] SKELETON arm -- fb/audio wrong on purpose\n");
#endif

    }
#endif
#ifdef RIG_GBR_CENSUS
        {
            extern unsigned long long rig_gbr_hist[2][256];
            int c_, b_;
            unsigned long long tot_ = 0;
            for (c_ = 0; c_ < 2; c_++)
                for (b_ = 0; b_ < 256; b_++)
                    tot_ += rig_gbr_hist[c_][b_];
            printf("[32x-gbrcensus] total 0xC5 dispatches: %llu\n", tot_);
            for (c_ = 0; c_ < 2; c_++)
                for (b_ = 0; b_ < 256; b_++)
                    if (rig_gbr_hist[c_][b_])
                        printf("[32x-gbrcensus] %s gbr_top=%02x count=%llu (%.2f%%)\n",
                               c_ ? "ssh2" : "msh2", (unsigned)b_,
                               rig_gbr_hist[c_][b_],
                               100.0 * (double)rig_gbr_hist[c_][b_] / (double)tot_);
        }
#endif
#ifdef RIG_OPHIST
    /* Opcode-group census: what the frame is actually made of, per core.
     * Answers dispatch-shape questions with a distribution instead of a
     * guess (see the RIG_OPHIST comment in sh2pico.c). */
    {
        extern unsigned int rig_ophist[2][256];
        for (int c = 0; c < 2; c++) {
            unsigned long long tot = 0;
            unsigned idx[256];
            for (int i = 0; i < 256; i++) { tot += rig_ophist[c][i]; idx[i] = i; }
            if (!tot) continue;
            /* simple selection of the top 16 */
            printf("[32x-ophist] %s: %llu insns, top 16 opcode groups (op>>8):\n",
                   c ? "ssh2" : "msh2", tot);
            /* Group 0xC in full: the opcode-cost census left it as the only
             * msh2 outlier (avg 3.3 ticks against 0.6-1.1 elsewhere) and
             * op1100 switches on exactly this nibble, so the row names the
             * op. */
            {
                static const char *c_nm[16] = {
                  "MOV.B R0,@(d,GBR)","MOV.W R0,@(d,GBR)","MOV.L R0,@(d,GBR)","TRAPA #imm",
                  "MOV.B @(d,GBR),R0","MOV.W @(d,GBR),R0","MOV.L @(d,GBR),R0","MOVA @(d,PC),R0",
                  "TST #imm,R0","AND #imm,R0","XOR #imm,R0","OR #imm,R0",
                  "TST.B #imm,@(R0,GBR)","AND.B #imm,@(R0,GBR)","XOR.B #imm,@(R0,GBR)","OR.B #imm,@(R0,GBR)" };
                unsigned long long ctot = 0;
                for (int i = 0xC0; i <= 0xCF; i++) ctot += rig_ophist[c][i];
                if (ctot)
                    for (int i = 0xC0; i <= 0xCF; i++)
                        if (rig_ophist[c][i])
                            printf("[32x-ophist]   0x%02x__ %-22s %9u  %5.2f%% of 0xC\n",
                                   i, c_nm[i & 0xf], rig_ophist[c][i],
                                   100.0 * rig_ophist[c][i] / (double)ctot);
            }
            for (int k = 0; k < 16; k++) {
                int best = -1;
                for (int i = 0; i < 256; i++) {
                    if (!rig_ophist[c][i]) continue;
                    if (best < 0 || rig_ophist[c][i] > rig_ophist[c][best]) best = i;
                }
                if (best < 0) break;
                printf("[32x-ophist]   0x%02x__  %10u  %5.2f%%\n",
                       best, rig_ophist[c][best],
                       100.0 * rig_ophist[c][best] / (double)tot);
                rig_ophist[c][best] = 0;
            }
            (void)idx;
        }
        {
            extern unsigned int rig_ophist_lo0[2][256];
            for (int c = 0; c < 2; c++) {
                unsigned long long tot = 0;
                for (int i = 0; i < 256; i++) tot += rig_ophist_lo0[c][i];
                if (!tot) continue;
                printf("[32x-ophist] %s 0x00xx breakdown (%llu insns), top 8:\n",
                       c ? "ssh2" : "msh2", tot);
                for (int k = 0; k < 8; k++) {
                    int best = -1;
                    for (int i = 0; i < 256; i++) {
                        if (!rig_ophist_lo0[c][i]) continue;
                        if (best < 0 || rig_ophist_lo0[c][i] > rig_ophist_lo0[c][best]) best = i;
                    }
                    if (best < 0) break;
                    printf("[32x-ophist]   op=0x00%02x  %10u  %5.2f%%\n",
                           best, rig_ophist_lo0[c][best],
                           100.0 * rig_ophist_lo0[c][best] / (double)tot);
                    rig_ophist_lo0[c][best] = 0;
                }
            }
        }
    }
#endif
#ifdef RIG_PP_CENSUS
    {
        extern unsigned char rig_pp_idx_seen[256];
        extern int rig_pp_md_l0, rig_pp_md_l1, rig_pp_md_x0, rig_pp_md_x1;
        extern unsigned long long rig_pp_md_px, rig_pp_px;
        int i, n = 0;
        for (i = 0; i < 256; i++) n += rig_pp_idx_seen[i];
        printf("[32x-pp] distinct 32X packed-pixel indices used: %d/256\n", n);
        printf("[32x-pp] MD non-background: lines %d..%d, x %d..%d, "
               "%llu of %llu px (%.2f%%)\n",
               rig_pp_md_l0, rig_pp_md_l1, rig_pp_md_x0, rig_pp_md_x1,
               rig_pp_md_px, rig_pp_px,
               rig_pp_px ? 100.0 * rig_pp_md_px / (double)rig_pp_px : 0.0);
    }
#endif
#ifdef RIG_M68K_HIST
    {
        struct rig_m68k_slot { unsigned int at, hits; };
        extern struct rig_m68k_slot rig_m68k_tab[1024];
        extern unsigned long long rig_m68k_insns, rig_m68k_evict;
        printf("[32x-m68k] %llu guest insns (%llu/frame), %llu evicted; top 16 PCs:\n",
               rig_m68k_insns, rig_m68k_insns / (unsigned)(RIG_FRAMES),
               rig_m68k_evict);
        for (int k = 0; k < 16; k++) {
            int best = -1;
            for (int i = 0; i < 1024; i++) {
                if (!rig_m68k_tab[i].hits) continue;
                if (best < 0 || rig_m68k_tab[i].hits > rig_m68k_tab[best].hits) best = i;
            }
            if (best < 0) break;
            printf("[32x-m68k]   0x%06x  %10u  %5.2f%%\n",
                   rig_m68k_tab[best].at, rig_m68k_tab[best].hits,
                   100.0 * rig_m68k_tab[best].hits / (double)rig_m68k_insns);
            rig_m68k_tab[best].hits = 0;
        }
    }
#endif
#ifdef RIG_SH2_PC_HIST
    rig_pchist_report();
#endif
#ifdef RIG_Z80_HIST
    rig_z80_pchist_report();
#endif
#ifdef RIG_POLL_PEEK
    rig_peek_report();
#endif
#ifdef RIG_SDRAM_POLL_DIAG
    rig_spd_report();
#endif
#ifdef GNW_S3_ARM
    {
        extern unsigned long long gnw_sh2_s3_folds;
        printf("[s3] arm=%d folds=%llu\n", GNW_S3_ARM, gnw_sh2_s3_folds);
    }
#endif
#ifdef GNW_S3_DIAG
    { extern unsigned long long gnw_sh2_s4_folds; /* GNW: shape4 record-walk fold counter (DIAG-only, see sh2pico.c) */
      extern unsigned long long gnw_sh2_s5_folds; /* GNW: shape5 command-poll fold counter */
      printf("[s4] record-walk folds=%llu\n", gnw_sh2_s4_folds);
      printf("[s5] cmd-poll folds=%llu\n", gnw_sh2_s5_folds); }
#endif
#ifdef GNW_S3_SETDIAG
    {
        extern unsigned long long gnw_sh2_s3_sd[4];
        extern unsigned int gnw_sh2_s3_sd_old_pc[16];
        extern int gnw_sh2_s3_sd_old_n;
        extern unsigned int gnw_sh2_s3_sd_new_pc[16];
        extern int gnw_sh2_s3_sd_new_n;
        int i;
        printf("[s3sd] matcher-dispatch fold-set buckets: both=%llu old-only=%llu new-only=%llu neither=%llu\n",
            gnw_sh2_s3_sd[0], gnw_sh2_s3_sd[1], gnw_sh2_s3_sd[2], gnw_sh2_s3_sd[3]);
        printf("[s3sd] old-only PCs (s3-era over-fold risk):");
        for (i = 0; i < gnw_sh2_s3_sd_old_n; i++)
            printf(" %08x", gnw_sh2_s3_sd_old_pc[i]);
        printf("\n[s3sd] new-only PCs:");
        for (i = 0; i < gnw_sh2_s3_sd_new_n; i++)
            printf(" %08x", gnw_sh2_s3_sd_new_pc[i]);
        printf("\n");
    }
#endif
#ifdef GNW_SH2_DRC
    {
        /* GNW: per-core counters — the shared gnw_sh2_drc_runs counter only
         * increments in the STUB diagnostic branch; native execution bumps
         * runs_m/runs_s instead (team lead 2026-09-26: never read runs=0 as
         * "not executing"). Coverage = lookups - misses - stub-only runs. */
        extern unsigned long long gnw_sh2_drc_lookups, gnw_sh2_drc_runs,
            gnw_sh2_drc_trans, gnw_sh2_drc_flushes, gnw_sh2_drc_misses,
            gnw_sh2_drc_runs_m, gnw_sh2_drc_runs_s;
        printf("[32x-qemu] DRC lookups=%llu runs(stub-only)=%llu "
               "runs_m=%llu runs_s=%llu trans=%llu misses=%llu flushes=%llu\n",
               gnw_sh2_drc_lookups, gnw_sh2_drc_runs,
               gnw_sh2_drc_runs_m, gnw_sh2_drc_runs_s,
               gnw_sh2_drc_trans, gnw_sh2_drc_misses, gnw_sh2_drc_flushes);
    }
#endif
    phase_report(n, ipt_x1000, n > 0 ? sh2_tot / n : 0);
#ifdef RIG_YM_CENSUS
    ym_census_report();
#endif
    printf("[32x-qemu] snd hash=%08lx calls=%u samples=%u\n",
           (unsigned long)s_snd_hash, s_snd_calls, s_snd_samples);
    printf("[32x-qemu] snd energy sum|s|=%llu sum s^2=%llu peak=%d\n",
           s_snd_abs, s_snd_sq, s_snd_peak);
    printf("[32x-qemu] fb f%d=%08lx(nb=%d) f%d=%08lx(nb=%d) f%d=%08lx(nb=%d)\n",
           CK_A, (unsigned long)cks100, nb100, CK_B, (unsigned long)cks300, nb300,
           CK_LAST, (unsigned long)cksend, nbend);

    /* The two LATE samples must differ. The old test accepted
     * (cks100 != cks300 || cks300 != cksend), and the first half of that is
     * satisfied by going from a blank screen to a still one -- which is exactly
     * what a fatal error looks like. D32XR sat on "Z_Malloc: failed on 496",
     * both SH-2s busy in the interrupt handler, and this gate called it PASS
     * for as long as it existed; the run was reported upward as "the rig cannot
     * reproduce the device failure". It had been reproducing it all along. A
     * checksum cannot read, so do not ask it whether the screen is good --
     * only whether it is still moving. */
    int moving = (cks300 != cksend);
    int pass = nbend && moving && sh2_tot > 0;
    if (pass) {
        printf("[32x-qemu] GATE3 PASS: frames advance, fb alive and moving, SH-2s executing\n");
    } else {
        printf("[32x-qemu] GATE3 FAIL: %s\n",
               !nbend  ? "framebuffer blank at the end" :
               !moving ? "framebuffer FROZEN between the last two samples -- if a game "
                         "is running this is a death screen; dump it with "
                         "EXTRA_DEF=\"-DRIG_FB_DUMP=<frame>\" and "
                         "tools/m7_qemu_rig/fbdump_to_png.py, and READ it"
                       : "SH-2s never executed");
    }
#ifdef RIG_SRAM_FILL
    {
        extern void *rig_sram_ptr(u32 *size);
        u32 sv_size = 0;
        unsigned char *sv = rig_sram_ptr(&sv_size);
        if (sv && sv_size) {
            u32 nz = 0, first_nz = 0;
            int seen = 0;
            for (u32 i = 0; i < sv_size; i++)
                if (sv[i]) { nz++; if (!seen) { first_nz = i; seen = 1; } }
            printf("[sram] end state: %u/%u non-zero, first at 0x%x\n",
                   nz, sv_size, first_nz);
            if (s_sv_dump_hex) {
                for (u32 off = 0; off < sv_size; off += 32) {
                    printf("[svhex %04x]", off);
                    for (u32 j = 0; j < 32 && off + j < sv_size; j++)
                        printf("%02x", sv[off + j]);
                    printf("\n");
                }
                printf("[sram] dumped %u bytes as [svhex] lines\n", sv_size);
            }
        }
    }
#endif
    return pass ? 0 : 4;
}
