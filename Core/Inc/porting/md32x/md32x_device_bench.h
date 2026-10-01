#ifndef MD32X_DEVICE_BENCH_H
#define MD32X_DEVICE_BENCH_H
/* Measurement builds only. Window boundaries are completed PicoFrame calls,
 * including audio pacing. No SD writes or debugger reads occur in the window. */
#if defined(GNW_32X_DEVICE_BENCH_FRAMES)
#include <stdint.h>
#include <stdbool.h>
#ifdef MD32X_DEVICE_PROFILE
#error "Disable the phase profiler for finite FPS measurement"
#endif
#ifndef GNW_32X_DEVICE_BENCH_WARMUP
#define GNW_32X_DEVICE_BENCH_WARMUP 1200
#endif
#ifdef GNW_32X_DEVICE_BENCH_START_FRAME
#ifndef GNW_32X_DEVICE_BENCH_START_FRAMES
#define GNW_32X_DEVICE_BENCH_START_FRAMES 8
#endif
#ifndef GNW_32X_DEVICE_BENCH_START_PAD
#define GNW_32X_DEVICE_BENCH_START_PAD (1u << 7)
#endif
_Static_assert(GNW_32X_DEVICE_BENCH_START_FRAME >= 0 && GNW_32X_DEVICE_BENCH_START_FRAMES > 0,
               "invalid startup input pulse");
_Static_assert((uint64_t)GNW_32X_DEVICE_BENCH_START_FRAME + GNW_32X_DEVICE_BENCH_START_FRAMES <= GNW_32X_DEVICE_BENCH_WARMUP,
               "startup input must finish before the measurement window");
#endif
#ifdef GNW_32X_DEVICE_BENCH_CONFIRM_FRAME
#ifndef GNW_32X_DEVICE_BENCH_CONFIRM_FRAMES
#define GNW_32X_DEVICE_BENCH_CONFIRM_FRAMES 8
#endif
#ifndef GNW_32X_DEVICE_BENCH_CONFIRM_PAD
#define GNW_32X_DEVICE_BENCH_CONFIRM_PAD (1u << 4)
#endif
_Static_assert(GNW_32X_DEVICE_BENCH_CONFIRM_FRAME >= 0 && GNW_32X_DEVICE_BENCH_CONFIRM_FRAMES > 0,
               "invalid confirmation input pulse");
_Static_assert((uint64_t)GNW_32X_DEVICE_BENCH_CONFIRM_FRAME + GNW_32X_DEVICE_BENCH_CONFIRM_FRAMES <= GNW_32X_DEVICE_BENCH_WARMUP,
               "confirmation input must finish before the measurement window");
#endif
#ifdef GNW_32X_DEVICE_BENCH_CONFIRM2_FRAME
#ifndef GNW_32X_DEVICE_BENCH_CONFIRM_FRAME
#error "second confirmation requires the first confirmation pulse"
#endif
_Static_assert(GNW_32X_DEVICE_BENCH_CONFIRM2_FRAME >= 0 &&
               (uint64_t)GNW_32X_DEVICE_BENCH_CONFIRM2_FRAME + GNW_32X_DEVICE_BENCH_CONFIRM_FRAMES <= GNW_32X_DEVICE_BENCH_WARMUP,
               "second confirmation must finish before the measurement window");
#endif
_Static_assert(GNW_32X_DEVICE_BENCH_FRAMES > 0, "empty benchmark window");
_Static_assert(GNW_32X_DEVICE_BENCH_WARMUP >= 0, "negative warmup");
_Static_assert((uint64_t)GNW_32X_DEVICE_BENCH_WARMUP + GNW_32X_DEVICE_BENCH_FRAMES < UINT32_MAX,
               "benchmark frame counter overflow");
struct gnw_md32x_bench_record {
    uint32_t magic, version, warmup, window, completed, done;
    uint32_t start_ms, end_ms, start_emu, end_emu, start_drawn, end_drawn;
    uint32_t audio_calls, audio_samples, muted_calls;
    uint32_t start_audio_calls, start_audio_samples, start_muted_calls;
    uint32_t end_audio_calls, end_audio_samples, end_muted_calls;
    uint32_t input_or, clock_hz, last_drawn_frame, framebuffer;
    uint32_t fullscreen, tear_guard;
};
volatile struct gnw_md32x_bench_record gnw_md32x_bench_result
    __attribute__((aligned(32)));
extern void common_emu_bench_begin(void);
extern void common_emu_bench_complete(void);

static inline void gnw_md32x_bench_begin(void)
{
    gnw_md32x_bench_result.start_emu = g_common_emu_frames;
    gnw_md32x_bench_result.start_drawn = g_common_drawn_frames;
    gnw_md32x_bench_result.start_audio_calls = gnw_md32x_bench_result.audio_calls;
    gnw_md32x_bench_result.start_audio_samples = gnw_md32x_bench_result.audio_samples;
    gnw_md32x_bench_result.start_muted_calls = gnw_md32x_bench_result.muted_calls;
    gnw_md32x_bench_result.input_or = 0;
    common_emu_bench_begin(); /* live smoke only: validate the wall timer */
    gnw_md32x_bench_result.start_ms = HAL_GetTick();
}
static inline void gnw_md32x_bench_init(void)
{
    gnw_md32x_bench_result = (struct gnw_md32x_bench_record){
        .magic = 0x32425844u, .version = 1,
        .warmup = GNW_32X_DEVICE_BENCH_WARMUP,
        .window = GNW_32X_DEVICE_BENCH_FRAMES
    };
    if (!GNW_32X_DEVICE_BENCH_WARMUP) gnw_md32x_bench_begin();
}
static inline void gnw_md32x_bench_audio(uint32_t samples, bool muted)
{
    gnw_md32x_bench_result.audio_calls++;
    gnw_md32x_bench_result.audio_samples += samples;
    gnw_md32x_bench_result.muted_calls += muted;
}
static inline bool gnw_md32x_bench_tick(bool drawn, uint32_t input)
{
    if (gnw_md32x_bench_result.done) return false;
    uint32_t n = ++gnw_md32x_bench_result.completed;
    if (drawn) gnw_md32x_bench_result.last_drawn_frame = n;
    if (n > GNW_32X_DEVICE_BENCH_WARMUP)
        gnw_md32x_bench_result.input_or |= input;
    if (n == GNW_32X_DEVICE_BENCH_WARMUP) gnw_md32x_bench_begin();
    if (n != GNW_32X_DEVICE_BENCH_WARMUP + GNW_32X_DEVICE_BENCH_FRAMES) return false;
    gnw_md32x_bench_result.end_ms = HAL_GetTick();
    gnw_md32x_bench_result.end_emu = g_common_emu_frames;
    gnw_md32x_bench_result.end_drawn = g_common_drawn_frames;
    gnw_md32x_bench_result.end_audio_calls = gnw_md32x_bench_result.audio_calls;
    gnw_md32x_bench_result.end_audio_samples = gnw_md32x_bench_result.audio_samples;
    gnw_md32x_bench_result.end_muted_calls = gnw_md32x_bench_result.muted_calls;
    gnw_md32x_bench_result.done = 1;
    return true;
}
#endif
#endif
