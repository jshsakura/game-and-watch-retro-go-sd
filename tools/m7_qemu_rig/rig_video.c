/* MJPEG-AVI video player on the M7 QEMU rig: reproduces the PROGRESSIVE
 * SLOWDOWN of long playback deterministically, on a real ARMv7-M instruction
 * stream, by making the two clocks that actually cause it VISIBLE and separate.
 *
 * The diagnosis (see Core/Src/porting/video/CLAUDE.md, "Nothing synchronises
 * the two clocks"): video paces on SysTick (HAL_GetTick); audio is drained by
 * the SAI ISR at the audio PLL's real 48 kHz — a SECOND, INDEPENDENT clock.
 * trim_step() is a pure proportional servo with only +/-1% authority, so a
 * mismatch beyond that cannot be cancelled and the ring fill climbs
 * monotonically. When it saturates, the prefetch gate latches shut and every
 * later frame pays an un-prefetched blocking read — the cliff. A seek resets it.
 *
 * This rig links the REAL firmware TUs unchanged — video_play.c, avi.c,
 * video_decode.c, video_audio.c and the real minimp3 (music_minimp3.c) — and
 * drives the real video_play() loop to the end of a long synthetic clip. The
 * two clocks are kept separate ON PURPOSE:
 *
 *   VIDEO PACING CLOCK  = HAL_GetTick(), advanced ONLY by the injected latency
 *                         models: __wrap_fread (SD read), __wrap_fseek (seek),
 *                         the JPEG decode stub, and HAL_Delay.
 *   AUDIO CONSUME CLOCK = a fake SAI ISR (isr_audio_pump) that advances the ring
 *                         TAIL at AUDIO_HZ = 48000 * (1 - AUDIO_PPM/1e6) samples
 *                         per second of virtual video time, via the g_tail
 *                         pointer captured in the music_attach() shim.
 *
 * With AUDIO_PPM = 0 the two rates match exactly (the clip is muxed so one
 * displayed video frame produces exactly one 48 kHz MP3 frame's worth of
 * samples, and usec_per_frame is chosen so 48000*usec/1e6 == 1152) and the ring
 * stays flat — the control that proves the rig is not simply always-broken. Any
 * AUDIO_PPM > 0 makes the SAI drain slower than the demuxer fills, so the ring
 * climbs; past the servo's +/-1% authority (~10000 ppm) it never comes back and
 * the gate latches. That is the reproduction.
 *
 * NOTE ON SIGN (deviation from the plan's literal "48000*(1+ppm)"): the bug is
 * the ring FILLING toward 4095 and latching the *full*-side prefetch gate. That
 * needs consumption < production. So AUDIO_PPM > 0 here models the audio clock
 * running SLOW by that many ppm — AUDIO_HZ = 48000*(1 - ppm/1e6). Same two-clock
 * physics, same magnitudes; only the sign convention is flipped so the positive
 * sweep values reproduce the documented climb-to-4095 latch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "avi.h"
#include "video_play.h"
#include "video_decode.h"
#include "video_audio.h"

/* ---- rig_runtime.c ---- */
void rig_timer_init(void);

/* ================= knobs (override with -D on the compile line) ============= */
#ifndef RIG_FRAMES
#define RIG_FRAMES         6000      /* video frames in the synthetic clip */
#endif
#ifndef AUDIO_PPM
#define AUDIO_PPM          0         /* audio-clock SLOW-ness in ppm (see header) */
#endif
#ifndef RIG_WINDOW
#define RIG_WINDOW         200       /* ledger line every N frame-attempts */
#endif

/* Pacing: 24000 us/frame (41.667 fps) is chosen so nominal audio production
 * (one 48 kHz MP3 frame = 1152 samples per video frame) exactly equals nominal
 * consumption (48000 * 0.024 = 1152). That makes AUDIO_PPM=0 a mathematically
 * flat control. (The plan suggested ~33367 us for 29.97 fps; that rate does not
 * divide evenly into 1152-sample MP3 frames, so ppm=0 would carry a baseline
 * imbalance and muddy the control. The two-clock mechanism is unchanged.) */
#ifndef USEC_PER_FRAME
#define USEC_PER_FRAME     24000
#endif

/* SD read/seek/decode latency models (microseconds / bytes-per-microsecond). */
#ifndef READ_SETUP_US
#define READ_SETUP_US      60.0      /* per-fread fixed overhead */
#endif
#ifndef THROUGHPUT_BPUS
#define THROUGHPUT_BPUS    2.0       /* SD throughput, bytes per microsecond (=2 MB/s) */
#endif
#ifndef DECODE_US
#define DECODE_US          2000      /* HW JPEG decode per frame */
#endif
#ifndef SEEK_SETUP_US
#define SEEK_SETUP_US      30.0      /* per-fseek fixed overhead */
#endif
#ifndef SEEK_MODE
#define SEEK_MODE          0         /* 0=fixed 1=distance-proportional 2=abs-offset-proportional */
#endif
#ifndef SEEK_TP_BPUS
#define SEEK_TP_BPUS       50.0      /* seek "speed" for modes 1/2 */
#endif

/* Bursty MJPEG frame sizes: mostly small, a big frame every BURST_EVERY. The
 * prefetcher hides the bursts while it is running; once the gate latches they
 * become blocking reads that overrun the budget — the visible cliff. All <=
 * VIDEO_FRAME_MAX (64 KB). Audio chunks are the constant 384-byte MP3 frames. */
#ifndef BASE_SV
#define BASE_SV            8000
#endif
#ifndef BURST_SV
#define BURST_SV           56000
#endif
#ifndef BURST_EVERY
#define BURST_EVERY        24
#endif
#define AUDIO_SV           384       /* one CBR 48 kHz mono MP3 frame */

/* ================= embedded MP3 (objcopy -I binary) ========================= */
extern const uint8_t _binary_tone48_mp3_start[];
extern const uint8_t _binary_tone48_mp3_end[];

/* ================= virtual video clock ===================================== */
static uint64_t g_virtual_us = 1000;           /* start >0 so HAL_GetTick()!=0 */

/* fake SAI ISR: drain the ring tail to match elapsed virtual video time. */
static volatile uint16_t *g_isr_head, *g_isr_tail;
#define VR_SIZE_RIG 4096
#define VR_MASK_RIG (VR_SIZE_RIG - 1)
static double   g_drain_accum;                 /* fractional samples owed */
static uint64_t g_drain_last_us;
static double   g_audio_hz = 48000.0 * (1.0 - (double)(AUDIO_PPM) / 1e6);

static void isr_audio_pump(void)
{
    if (!g_isr_tail) return;                    /* ring not attached yet */
    uint64_t now = g_virtual_us;
    if (now <= g_drain_last_us) return;
    double dt_s = (double)(now - g_drain_last_us) / 1e6;
    g_drain_last_us = now;
    g_drain_accum += dt_s * g_audio_hz;
    int owe = (int)g_drain_accum;
    if (owe <= 0) return;
    g_drain_accum -= owe;
    int avail = ((int)*g_isr_head - (int)*g_isr_tail) & VR_MASK_RIG;   /* SAI cannot drain past head */
    if (owe > avail) owe = avail;
    *g_isr_tail = (uint16_t)((*g_isr_tail + owe) & VR_MASK_RIG);
}

/* the ONLY place virtual time grows: advance, then let the SAI drain to match. */
static void advance_clock(uint64_t us) { g_virtual_us += us; isr_audio_pump(); }

uint32_t HAL_GetTick(void) { isr_audio_pump(); return (uint32_t)(g_virtual_us / 1000); }
void     HAL_Delay(uint32_t ms) { advance_clock((uint64_t)ms * 1000); }

/* ================= hardware / UI seams (bodies lifted from
 * tests/test_video_play.c; only the ones that must MODEL something differ) === */
void wdog_refresh(void) {}
void SCB_CleanDCache_by_Addr(uint32_t *addr, int32_t dsize) { (void)addr; (void)dsize; }

static uint16_t s_fb[320 * 240];
uint16_t *lcd_get_active_buffer(void) { return s_fb; }
static long g_swaps;                            /* presented (non-dropped) frames */
static int  g_last_trough;                      /* ring level at end of the pacing wait */
/* lcd_swap() fires once per PRESENTED frame, right after the pacing wait has run
 * — the ring is at its per-frame TROUGH here (fully drained for this frame,
 * before the next frame's audio feed). Whether that trough falls back below the
 * gate-close level is exactly "did the prefetch gate reopen this frame". */
void lcd_swap(void) { g_swaps++; g_last_trough = video_audio_ring_count(); }

void music_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail)
{ (void)ring; (void)size; g_isr_head = head; g_isr_tail = tail; g_drain_last_us = g_virtual_us; g_drain_accum = 0; }
void music_audio_enable(int on) { (void)on; }
void music_audio_set(int vol, int play) { (void)vol; (void)play; }
void audio_start_playing(uint16_t length) { (void)length; }
void audio_stop_playing(void) {}

uint8_t common_emu_sound_get_volume(void) { return 8; }
#include "common.h"
common_emu_state_t common_emu_state;

uint32_t JPEG_DecodeToFrameInit(uint32_t buf, uint32_t sz) { (void)buf; (void)sz; return 0; }
uint32_t JPEG_DecodeDeInit(void) { return 0; }
uint32_t JPEG_DecodeToFrame(uint32_t src, uint32_t sz, uint32_t dst, uint16_t x, uint16_t y, uint8_t la)
{ (void)src; (void)sz; (void)dst; (void)x; (void)y; (void)la; advance_clock(DECODE_US); return 0; }

#define SCRATCH_MAX (352 * 1024)
uint8_t g_scratch[SCRATCH_MAX];

int  odroid_audio_volume_get(void) { return 5; }
void odroid_audio_volume_set(int level) { (void)level; }

#include "odroid_overlay.h"
int odroid_overlay_settings_menu(odroid_dialog_choice_t *extra, void_callback_t repaint, odroid_menu_flags_t flags)
{ (void)extra; (void)repaint; (void)flags; return -1; }

#include "rg_i18n.h"
static const lang_t s_lang = { .s_info = "Info", .s_Quit_to_menu = "Quit to menu" };
const lang_t *curr_lang = &s_lang;
int i18n_draw_text_line(uint16_t x, uint16_t y, uint16_t w, const char *t, uint16_t c, uint16_t bg, char f)
{ (void)x; (void)y; (void)w; (void)t; (void)c; (void)bg; (void)f; return 0; }
int i18n_get_text_width(const char *t) { return (int)strlen(t) * 6; }

#include "gui.h"
static colors_t s_colors = { 0x0000, 0xFFFF, 0xF800, 0x7BEF };
colors_t *curr_colors = &s_colors;

bool rg_alarm_poll(void) { return false; }

/* video_play.c declares these extern (not via a header). */
uint32_t g_jpeg_hal, g_jpeg_err, g_jpeg_rej, g_jpeg_sub, g_jpeg_need;

/* video_decode.c's split-timing globals we sample for the ledger. */
extern int g_vdec_read_ms, g_vdec_pf_ms, g_vdec_st;

/* ================= the synthetic AVI as a VIRTUAL file ======================
 * No megabytes are materialised: __wrap_fread synthesises bytes on demand from
 * a 100-byte header prefix + a regular movi body (per unit: one "00dc" video
 * chunk of sv_of(i) bytes, then one "01wb" 384-byte MP3 chunk). Frame sizes
 * vary (bursts), so a cumulative-offset table maps a file offset to its unit. */
#define HDR_LEN 100
static uint8_t  g_hdr[HDR_LEN];
static long    *g_cum;                          /* g_cum[i] = movi-relative start of unit i, [0..N] */
static long     g_file_end;
static int      g_nframes;
static int      g_mp3_nframes;                  /* frames available in the embedded MP3 */

/* minimal baseline JPEG header: SOI, SOF0 with h=240 w=320 — enough for
 * video_decode.c's jpeg_dims() SOF parse to pass. */
static const uint8_t JPEG_TMPL[] = {
    0xFF, 0xD8,                                 /* SOI */
    0xFF, 0xC0, 0x00, 0x11, 0x08,               /* SOF0, len=17, precision=8 */
    0x00, 0xF0,                                 /* height = 240 */
    0x01, 0x40,                                 /* width  = 320 */
    0x03, 0x01, 0x22, 0x00,                     /* 3 comps (padding, unread) */
    0xFF, 0xD9                                  /* EOI */
};
#define JPEG_TMPL_LEN ((long)sizeof(JPEG_TMPL))

static long sv_of(int i) { return ((i % BURST_EVERY) == (BURST_EVERY - 1)) ? BURST_SV : BASE_SV; }
static long unit_len(int i) { return 8 + sv_of(i) + 8 + AUDIO_SV; }

static void put_le32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void build_layout(void)
{
    g_nframes = RIG_FRAMES;
    g_mp3_nframes = (int)((_binary_tone48_mp3_end - _binary_tone48_mp3_start) / AUDIO_SV);
    if (g_mp3_nframes < 1) g_mp3_nframes = 1;

    g_cum = malloc(sizeof(long) * (g_nframes + 1));
    g_cum[0] = 0;
    for (int i = 0; i < g_nframes; i++) g_cum[i + 1] = g_cum[i] + unit_len(i);
    long movi_body = g_cum[g_nframes];
    g_file_end = HDR_LEN + movi_body;

    /* header prefix: RIFF/AVI /LIST hdrl(avih)/LIST movi */
    uint8_t *h = g_hdr;
    memcpy(h + 0, "RIFF", 4);
    put_le32(h + 4, (uint32_t)(g_file_end - 8));
    memcpy(h + 8, "AVI ", 4);
    memcpy(h + 12, "LIST", 4);
    put_le32(h + 16, 68);                        /* hdrl size: "hdrl"+avih(8+56) */
    memcpy(h + 20, "hdrl", 4);
    memcpy(h + 24, "avih", 4);
    put_le32(h + 28, 56);
    memset(h + 32, 0, 56);
    put_le32(h + 32 + 0,  USEC_PER_FRAME);       /* dwMicroSecPerFrame */
    put_le32(h + 32 + 16, (uint32_t)g_nframes);  /* dwTotalFrames */
    put_le32(h + 32 + 32, 320);                  /* dwWidth */
    put_le32(h + 32 + 36, 240);                  /* dwHeight */
    memcpy(h + 88, "LIST", 4);
    put_le32(h + 92, (uint32_t)(4 + movi_body)); /* movi size: "movi"+body */
    memcpy(h + 96, "movi", 4);
}

/* largest u with g_cum[u] <= rel  (rel in [0, g_cum[N]) ) */
static int find_unit(long rel)
{
    int lo = 0, hi = g_nframes;                  /* answer in [0, N-1] */
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (g_cum[mid] <= rel) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Fill n bytes of the virtual file starting at absolute offset `off`. */
static void vfill(long off, uint8_t *dst, long n)
{
    while (n > 0) {
        long take;
        if (off < HDR_LEN) {
            take = HDR_LEN - off; if (take > n) take = n;
            memcpy(dst, g_hdr + off, (size_t)take);
        } else {
            long rel = off - HDR_LEN;
            int  u   = find_unit(rel);
            long w   = rel - g_cum[u];
            long sv  = sv_of(u);
            long vlen = 8 + sv;
            if (w < vlen) {                       /* video chunk */
                if (w < 4) {
                    static const char cc[4] = { '0','0','d','c' };
                    take = 4 - w; if (take > n) take = n;
                    memcpy(dst, cc + w, (size_t)take);
                } else if (w < 8) {
                    uint8_t s4[4]; put_le32(s4, (uint32_t)sv);
                    take = 8 - w; if (take > n) take = n;
                    memcpy(dst, s4 + (w - 4), (size_t)take);
                } else {
                    long pw = w - 8;              /* payload index 0..sv-1 */
                    if (pw < JPEG_TMPL_LEN) {
                        take = JPEG_TMPL_LEN - pw;
                        if (take > vlen - w) take = vlen - w;
                        if (take > n) take = n;
                        memcpy(dst, JPEG_TMPL + pw, (size_t)take);
                    } else {
                        take = vlen - w; if (take > n) take = n;
                        memset(dst, 0, (size_t)take);   /* padding after the header */
                    }
                }
            } else {                              /* audio chunk */
                long aw = w - vlen;
                if (aw < 4) {
                    static const char cc[4] = { '0','1','w','b' };
                    take = 4 - aw; if (take > n) take = n;
                    memcpy(dst, cc + aw, (size_t)take);
                } else if (aw < 8) {
                    uint8_t s4[4]; put_le32(s4, AUDIO_SV);
                    take = 8 - aw; if (take > n) take = n;
                    memcpy(dst, s4 + (aw - 4), (size_t)take);
                } else {
                    long pa = aw - 8;             /* 0..383 */
                    const uint8_t *fr = _binary_tone48_mp3_start + (long)(u % g_mp3_nframes) * AUDIO_SV;
                    take = AUDIO_SV - pa; if (take > n) take = n;
                    memcpy(dst, fr + pa, (size_t)take);
                }
            }
        }
        off += take; dst += take; n -= take;
    }
}

/* ---- the wrapped stdio: one virtual file, tracked by a byte cursor ---- */
static long g_vf_pos;
static int  g_vf_open;

extern void  *__real_fopen(const char *, const char *);
extern size_t __real_fread(void *, size_t, size_t, void *);
extern int    __real_fseek(void *, long, int);
extern long   __real_ftell(void *);
extern void   __real_rewind(void *);
extern int    __real_fclose(void *);
extern int    __real_setvbuf(void *, char *, int, size_t);

#define VF ((void *)&g_vf_open)                  /* our sentinel FILE* */

void *__wrap_fopen(const char *path, const char *mode)
{ (void)path; (void)mode; g_vf_pos = 0; g_vf_open = 1; return VF; }

int __wrap_fclose(void *f)
{ if (f != VF) return __real_fclose(f); g_vf_open = 0; return 0; }

long __wrap_ftell(void *f)
{ if (f != VF) return __real_ftell(f); return g_vf_pos; }

void __wrap_rewind(void *f)
{ if (f != VF) { __real_rewind(f); return; } g_vf_pos = 0; }

int __wrap_setvbuf(void *f, char *buf, int mode, size_t sz)
{ if (f != VF) return __real_setvbuf(f, buf, mode, sz); return 0; }

int __wrap_fseek(void *f, long off, int whence)
{
    if (f != VF) return __real_fseek(f, off, whence);
    long np = whence == SEEK_SET ? off
            : whence == SEEK_CUR ? g_vf_pos + off
            : g_file_end + off;                  /* SEEK_END */
    double cost = SEEK_SETUP_US;
#if SEEK_MODE == 1
    long d = np - g_vf_pos; if (d < 0) d = -d;
    cost += (double)d / SEEK_TP_BPUS;
#elif SEEK_MODE == 2
    cost += (double)(np < 0 ? 0 : np) / SEEK_TP_BPUS;
#endif
    g_vf_pos = np;
    advance_clock((uint64_t)cost);
    return 0;
}

size_t __wrap_fread(void *dst, size_t size, size_t nmemb, void *f)
{
    if (f != VF) return __real_fread(dst, size, nmemb, f);
    long want = (long)(size * nmemb);
    long avail = g_file_end - g_vf_pos;
    if (avail < 0) avail = 0;
    if (want > avail) want = avail;
    if (want > 0) {
        vfill(g_vf_pos, (uint8_t *)dst, want);
        g_vf_pos += want;
        advance_clock((uint64_t)(READ_SETUP_US + (double)want / THROUGHPUT_BPUS));
    }
    return size ? (size_t)(want / (long)size) : 0;
}

/* ================= per-frame ledger =========================================
 * odroid_input_read_gamepad() is called exactly once at the top of every
 * video_play() loop iteration (i.e. once per frame ATTEMPT, presented or
 * dropped) — the natural per-frame hook. We sample the two clocks' visible
 * state here without touching the real code. */
#include "odroid_input.h"
#define PF_AUDIO_HEADROOM 2400                    /* mirrors video_play.c */
#define GATE_CLOSE_RING   (VR_SIZE_RIG - 1 - PF_AUDIO_HEADROOM)  /* ring_count that closes it = 1695 */
#define LATCH_TAIL_MIN    100                     /* frames closed at the end to call it a latch */

static long  g_attempt;
static long  g_win_dt_us, g_win_trough, g_win_rd, g_win_pf, g_win_stayed_closed;
static int   g_win_trough_min, g_win_trough_max, g_win_rd_max;
static long  g_win_swaps_base;
static uint64_t g_last_now;
/* events (all trough-based: the gate reopens iff the frame's trough drains back
 * below GATE_CLOSE_RING) */
static long  g_first_close = -1;                  /* first frame whose trough stayed above the close level */
static long  g_last_open   = -1;                  /* last frame whose trough fell back below it (gate reopened) */
static int   g_trough_peak;

static void ledger_window_reset(void)
{
    g_win_dt_us = g_win_trough = g_win_rd = g_win_pf = g_win_stayed_closed = 0;
    g_win_trough_min = 1 << 30; g_win_trough_max = 0; g_win_rd_max = 0;
    g_win_swaps_base = g_swaps;
}

void odroid_input_read_gamepad(odroid_gamepad_state_t *s)
{
    memset(s, 0, sizeof *s);
    if (!g_isr_tail) return;                       /* pre-start seed call: ring not attached */

    uint64_t now = g_virtual_us;
    long dt = (g_attempt == 0) ? 0 : (long)(now - g_last_now);
    g_last_now = now;

    int trough = g_last_trough;                    /* ring floor from the previous presented frame */
    bool stayed_closed = trough > GATE_CLOSE_RING;  /* gate never reopened that frame */

    if (stayed_closed && g_first_close < 0) g_first_close = g_attempt;
    if (!stayed_closed) g_last_open = g_attempt;
    if (trough > g_trough_peak) g_trough_peak = trough;

    g_win_dt_us += dt;
    g_win_trough += trough;
    g_win_rd    += g_vdec_read_ms;
    g_win_pf    += g_vdec_pf_ms;
    g_win_stayed_closed += stayed_closed ? 1 : 0;
    if (trough < g_win_trough_min) g_win_trough_min = trough;
    if (trough > g_win_trough_max) g_win_trough_max = trough;
    if (g_vdec_read_ms > g_win_rd_max) g_win_rd_max = g_vdec_read_ms;

    g_attempt++;
    if (g_attempt % RIG_WINDOW == 0) {
        long swaps = g_swaps - g_win_swaps_base;
        long drops = RIG_WINDOW - swaps; if (drops < 0) drops = 0;
        printf("f%05ld dt=%5ldus trough[avg=%4ld min=%4d max=%4d] gate_stayed_closed=%3ld/%d "
               "rd[avg=%2ldms max=%2dms] pf_avg=%2ldms drops=%ld\n",
               g_attempt,
               g_win_dt_us / RIG_WINDOW,
               g_win_trough / RIG_WINDOW, g_win_trough_min, g_win_trough_max,
               g_win_stayed_closed, RIG_WINDOW,
               g_win_rd / RIG_WINDOW, g_win_rd_max,
               g_win_pf / RIG_WINDOW, drops);
        ledger_window_reset();
    }
}

/* ================= main ===================================================== */
int main(void)
{
    rig_timer_init();
    build_layout();

    printf("[video-qemu] frames=%d usec/frame=%d AUDIO_PPM=%d audio_hz=%.3f\n",
           g_nframes, USEC_PER_FRAME, (int)(AUDIO_PPM), g_audio_hz);
    printf("[video-qemu] clip: file=%ld bytes  base_sv=%d burst_sv=%d/every=%d  mp3_frames=%d\n",
           g_file_end, (int)BASE_SV, (int)BURST_SV, (int)BURST_EVERY, g_mp3_nframes);
    printf("[video-qemu] model: read_setup=%.0fus tp=%.2fB/us decode=%dus seek_setup=%.0fus mode=%d\n",
           (double)READ_SETUP_US, (double)THROUGHPUT_BPUS, (int)DECODE_US, (double)SEEK_SETUP_US, (int)SEEK_MODE);
    printf("[video-qemu] gate closes at ring_count>%d (PF_AUDIO_HEADROOM=%d); VR_TARGET~1200\n",
           GATE_CLOSE_RING, PF_AUDIO_HEADROOM);

    ledger_window_reset();
    g_last_now = g_virtual_us;

    vid_result_t r = video_play("virtual.avi");

    long closed_tail = (g_last_open >= 0) ? (g_attempt - 1 - g_last_open) : g_attempt;
    printf("[video-qemu] video_play -> %d (0=OK 1=STOPPED 2=UNPLAYABLE)\n", (int)r);
    printf("[video-qemu] trough_peak=%d  first_gate_close=%ld  last_gate_reopen=%ld  closed_tail=%ld frames\n",
           g_trough_peak, g_first_close, g_last_open, closed_tail);
    if (g_first_close < 0)
        printf("[video-qemu] FLAT: gate reopened every frame — never latched (control)\n");
    else if (closed_tail >= LATCH_TAIL_MIN)
        printf("[video-qemu] LATCHED: gate stopped reopening at frame %ld and stayed shut "
               "for the final %ld frames — the cliff\n", g_last_open, closed_tail);
    else
        printf("[video-qemu] gate closed transiently but kept reopening (no permanent latch)\n");
    printf("[video-qemu] presented=%ld attempts=%ld drops=%ld\n",
           g_swaps, g_attempt, g_attempt - g_swaps);
    extern uint32_t g_video_audio_drops;
    printf("[video-qemu] valve_drops=%u samples\n", (unsigned)g_video_audio_drops);
    return 0;
}
