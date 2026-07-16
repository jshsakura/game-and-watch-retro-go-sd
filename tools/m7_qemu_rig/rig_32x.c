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
#ifdef RIG_TRACE_PC
#include <cpu/gwenesis68k/g68k.h>   /* the g68k global context (m68k.pc) */
#endif

#ifndef RIG_FRAMES
#define RIG_FRAMES 600
#endif
#define RIG_WARMUP 20

/* rig_runtime.c */
void rig_timer_init(void);
uint32_t rig_timer_now(void);
uint32_t rig_calibrate(uint32_t n);

/* SH-2 executed-instruction counter (fork: cpu/sh2/mame/sh2pico.c under
 * -DRIG_SH2_COUNT). Proves the SH-2s actually run — 0 here was the old hang's
 * signature (68000 spinning on a dead SH-2). */
extern unsigned long long g_sh2_insns;

/* ROM blob (objcopy .rom section) — already 16-bit byteswapped */
extern const unsigned char _binary_rom_32x_start[];
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

static int state_roundtrip_test(void) {
    struct rig_state_stream stream = {
        .data = malloc(RIG_STATE_CAP),
        .capacity = RIG_STATE_CAP,
    };
    if (stream.data == NULL) {
        printf("[32x-state] FAIL: buffer allocation\n");
        return -1;
    }

    for (int f = 0; f < 120; f++) {
        PicoIn.skipFrame = 0;
        PicoIn.pad[0] = pad_script(f);
        PicoFrame();
    }
    int save_ret = PicoStateFP(&stream, 1, rig_state_read, rig_state_write,
                               rig_state_eof, rig_state_seek);

    for (int f = 120; f < 150; f++) {
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
    for (int f = 120; f < 150; f++) {
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
    PicoIn.opt = POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
               | POPT_EN_32X | POPT_EN_PWM
               | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;   /* mono: no EN_STEREO */
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184;   /* US, EU, JP */

    enum media_type_e mt = PicoLoadMedia("game.32x", _binary_rom_32x_start, rom_len,
                                         NULL, NULL, NULL, NULL);
    printf("[32x-qemu] PicoLoadMedia -> media_type=%d AHW=%x\n", (int)mt, (unsigned)PicoIn.AHW);
    if (mt == PM_ERROR) { printf("[32x-qemu] LOAD FAILED\n"); return 3; }

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
        if (f == RIG_WARMUP) phase_snapshot();
        int skipf = skip_this_frame(f);
        PicoIn.skipFrame = (unsigned short)skipf;
        PicoIn.pad[0] = pad_script(f);
        uint32_t t0 = rig_timer_now();
        PicoFrame();
        uint32_t t1 = rig_timer_now();
        uint64_t insn = (uint64_t)(uint32_t)(t1 - t0) * ipt_x1000 / 1000;

        unsigned long long sh2_now = g_sh2_insns, sh2_d = sh2_now - sh2_prev;
        sh2_prev = sh2_now;

        if ((f % 20) == 0)
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
    phase_report(n, ipt_x1000, n > 0 ? sh2_tot / n : 0);
    printf("[32x-qemu] fb f%d=%08lx(nb=%d) f%d=%08lx(nb=%d) f%d=%08lx(nb=%d)\n",
           CK_A, (unsigned long)cks100, nb100, CK_B, (unsigned long)cks300, nb300,
           CK_LAST, (unsigned long)cksend, nbend);

    int pass = (nb100 || nb300 || nbend) &&
               (cks100 != cks300 || cks300 != cksend) &&
               sh2_tot > 0;
    printf("[32x-qemu] %s\n", pass ? "GATE3 PASS: frames advance, fb alive, SH-2s executing"
                                   : "GATE3 FAIL: blank/frozen fb or dead SH-2");
    return pass ? 0 : 4;
}
