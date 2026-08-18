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
#ifdef RIG_STATE_TEST
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
#define RIG_WARMUP 20
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
#define RIG_MIX_FROM 400
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
#define RIG_PC_HIST_SLOTS 8192
struct rig_pc_slot {
    uint32_t pc;
    uint32_t occupied;
    uint16_t opcode;
    unsigned long long dir;
    unsigned long long dly;
};
extern struct rig_pc_slot rig_pchist[2][RIG_PC_HIST_SLOTS];

struct rig_pc_top { uint32_t pc; uint32_t core; uint16_t opcode; unsigned long long dir, dly, total; };

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
            top[n].dir = d; top[n].dly = dl; top[n].total = t;
            grand += t;
            n++;
        }
    }
    qsort(top, n, sizeof(*top), rig_pchist_cmp);
    unsigned shown = n < 50 ? n : 50;
    printf("[32x-pchist] %u unique PCs, %llu total guest SH-2 insns; top %u:\n",
           n, grand, shown);
    unsigned long long acc = 0;
    for (unsigned i = 0; i < shown; i++) {
        acc += top[i].total;
        char dasm[80] = "";
        DasmSH2(dasm, top[i].pc, top[i].opcode);
        printf("[32x-pchist]  #%-2u %-6s 0x%08x  dir=%-10llu dly=%-10llu  %5.2f%%  cum %5.2f%%  %s\n",
               i + 1, top[i].core ? "slave" : "master", top[i].pc,
               top[i].dir, top[i].dly,
               grand ? 100.0 * top[i].total / grand : 0.0,
               grand ? 100.0 * acc / grand : 0.0, dasm);
    }
    free(top);
}
#endif /* RIG_SH2_PC_HIST */

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
static void rig_write_sound(int len) { s_snd_calls++; s_snd_samples += (unsigned)len; }

/* ==== optional-mode helpers =============================================== */

/* PicoIn.pad format: MXYZ SACB RLDU (pico.h) */
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
    if (f >= 118 && f < 130) return PAD_START;
    if (f >= 200 && f < 212) return PAD_START;
    if (f >= 260) {
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
}
#else
static unsigned short pad_script(int f) { (void)f; return 0; }
#endif

#ifdef RIG_STATE_TEST
#define RIG_STATE_CAP (1024u * 1024u)

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
#endif

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

    /* ---- libretro (upstream frontend) init order — NOT the old device order.
     * 32X startup is LAZY: the game's own MD-mode boot code writes ADEN at
     * 0xA15101 and PicoWrite8_32x calls Pico32xStartup (which itself runs
     * Pico32xPrepare + emu_32x_startup). Calling Pico32xStartup up front, the
     * old device/rig order, pre-enables the adapter and breaks the boot
     * handshake: VF's 68K parks in a nop/bra idle loop at 0x88088e forever.
     * PicoReset is not called either — PicoLoadMedia -> PicoCartInsert ->
     * PicoPower already reset the machine. */
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
#ifdef RIG_STRPAGE
        if (f == 8 && !s_strp_installed) { s_strp_installed = 1; rig_strp_install(); }
#endif
        if (f == RIG_WARMUP) phase_snapshot();
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
            extern unsigned long long gnw_mix[6][5];
            memset(gnw_mix, 0, sizeof(gnw_mix));
            printf("[32x-mix] census reset at frame %d (gameplay entry)\n", f);
        }
#endif
        int skipf = skip_this_frame(f);
        PicoIn.skipFrame = (unsigned short)skipf;
        PicoIn.pad[0] = pad_script(f);
        uint32_t t0 = rig_timer_now();
        PicoFrame();
        uint32_t t1 = rig_timer_now();
        uint64_t insn = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000;

        unsigned long long sh2_now = g_sh2_insns, sh2_d = sh2_now - sh2_prev;
        sh2_prev = sh2_now;

#ifdef RIG_SH2_WATCH
        /* D32XR hunt: bracket the frame where the guest SH-2s leave game
         * code for the BIOS re-entry. First watch showed both CPUs already
         * inside the BIOS checksum loop at f=160, so the exit happened
         * earlier: this edge detector logs the SDRAM->BIOS transition frame
         * itself. pc@0x40 ppc@0x44, sizeof(SH2)=6016. */
        if (f >= 4 && f < 160) {
            extern unsigned char sh2s[];
            static unsigned prev_m, prev_s;
            unsigned char *m = sh2s, *s = sh2s + 6016;
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
            extern unsigned char sh2s[];
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
            extern unsigned char sh2s[];
            const unsigned char *sd = (const unsigned char *)Pico32xMem;
            s_ds_done = 1;
            for (int c = 0; c < 2; c++) {
                const unsigned char *cpu = sh2s + c * 6016;
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
            unsigned r15 = *(const unsigned *)(sh2s + 0x3c);
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

        if ((f % 20) == 0 || (f >= 140 && f < 260))
            printf("  f%04d host=%lu sh2=%llu snd=%u/%u\n", f,
                   (unsigned long)insn, sh2_d, s_snd_calls, s_snd_samples);
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
        extern unsigned long long gnw_mix[6][5];
        static const char *const opn[6] = { "r8", "r16", "r32", "w8", "w16", "w32" };
        static const char *const rgn[5] = { "sdram", "rom", "dram", "cs0", "other" };
        unsigned long long grand = 0;
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 5; j++) grand += gnw_mix[i][j];
        printf("[32x-mix] SH-2 data accesses, total=%llu\n", grand);
        for (int i = 0; i < 6; i++) {
            unsigned long long row = 0;
            for (int j = 0; j < 5; j++) row += gnw_mix[i][j];
            if (!row) continue;
            printf("[32x-mix]  %-4s tot=%-12llu", opn[i], row);
            for (int j = 0; j < 5; j++)
                printf(" %s=%llu(%lu%%)", rgn[j], gnw_mix[i][j],
                       (unsigned long)(row ? gnw_mix[i][j] * 100 / row : 0));
            printf("  [%lu%% of all]\n",
                   (unsigned long)(grand ? row * 100 / grand : 0));
        }
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
#ifdef RIG_SH2_PC_HIST
    rig_pchist_report();
#endif
#ifdef RIG_POLL_PEEK
    rig_peek_report();
#endif
#ifdef RIG_SDRAM_POLL_DIAG
    rig_spd_report();
#endif
    phase_report(n, ipt_x1000, n > 0 ? sh2_tot / n : 0);
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
