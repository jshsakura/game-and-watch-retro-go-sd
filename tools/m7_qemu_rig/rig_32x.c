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
 * changing across frames, avg host insn/frame reported. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "pico/pico_types.h"
#include "pico/pico.h"

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

    uint64_t tot = 0, mn = ~0ull, mx = 0, sh2_tot = 0;
    unsigned long long sh2_prev = g_sh2_insns;
    uint32_t cks100 = 0, cks300 = 0, cksend = 0;
    int nb100 = 0, nb300 = 0, nbend = 0;

    for (int f = 0; f < RIG_FRAMES; f++) {
        uint32_t t0 = rig_timer_now();
        PicoIn.pad[0] = 0;
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
        }
        if (f == 99)  cks100 = fb_checksum(&nb100);
        if (f == 299) cks300 = fb_checksum(&nb300);
        if (f == RIG_FRAMES - 1) cksend = fb_checksum(&nbend);
    }

    int n = RIG_FRAMES - RIG_WARMUP;
    printf("[32x-qemu] done %d frames  avg host=%lu  min=%lu  max=%lu insn/frame  avg sh2=%llu\n",
           RIG_FRAMES, (unsigned long)(n > 0 ? tot / n : 0), (unsigned long)mn,
           (unsigned long)mx, (unsigned long long)(n > 0 ? sh2_tot / n : 0));
    printf("[32x-qemu] fb f100=%08lx(nb=%d) f300=%08lx(nb=%d) f%d=%08lx(nb=%d)\n",
           (unsigned long)cks100, nb100, (unsigned long)cks300, nb300,
           RIG_FRAMES, (unsigned long)cksend, nbend);

    int pass = (nb100 || nb300 || nbend) &&
               (cks100 != cks300 || cks300 != cksend) &&
               sh2_tot > 0;
    printf("[32x-qemu] %s\n", pass ? "GATE3 PASS: frames advance, fb alive, SH-2s executing"
                                   : "GATE3 FAIL: blank/frozen fb or dead SH-2");
    return pass ? 0 : 4;
}
