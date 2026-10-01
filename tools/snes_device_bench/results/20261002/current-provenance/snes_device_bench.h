/* Fixed-window hardware observation. Default OFF; no SD writes or progress polls. */
#pragma once
#define SNES_BENCH_WARMUP 120u
#define SNES_BENCH_FRAMES 900u

typedef struct {
  uint32_t magic, version, warmup, window, completed, done;
  uint32_t start_ms, end_ms, start_emu, end_emu, start_drawn, end_drawn;
  uint32_t audio_calls, audio_samples, muted_calls;
  uint32_t start_audio_calls, start_audio_samples, start_muted_calls;
  uint32_t end_audio_calls, end_audio_samples, end_muted_calls;
  uint32_t input_or, clock_hz, last_drawn_frame, framebuffer;
  uint32_t rom_crc32, rom_bytes, state_resumed, state_refused;
  uint32_t initial_state_crc32, start_state_crc32, end_state_crc32;
  uint32_t initial_guest_frame, start_guest_frame, end_guest_frame;
  uint32_t start_dma, end_dma, start_underruns, end_underruns;
  uint32_t audio_buffer_samples, gapfree, speedup;
} snes_device_bench_record_t;

volatile snes_device_bench_record_t snes_device_bench_result __attribute__((aligned(32)));
extern void common_emu_bench_begin(void);
extern void common_emu_bench_complete(void);

static uint32_t snes_device_state_crc;
static void snes_device_crc(void *ctx, void *data, size_t size) {
  (void)ctx;
  snes_device_state_crc = crc32_le(snes_device_state_crc, data, size);
}
static uint32_t snes_device_state_hash(void) {
  snes_device_state_crc = 0;
  state_stream(snes_device_crc);
  return snes_device_state_crc;
}
static void snes_device_bench_init(void) {
  memset((void *)&snes_device_bench_result, 0, sizeof(snes_device_bench_result));
  snes_device_bench_result.magic = 0x53425844u;
  snes_device_bench_result.version = 1;
  snes_device_bench_result.warmup = SNES_BENCH_WARMUP;
  snes_device_bench_result.window = SNES_BENCH_FRAMES;
  snes_device_bench_result.rom_crc32 = crc32_le(0, snes_rom, snes_rom_len);
  snes_device_bench_result.rom_bytes = snes_rom_len;
  snes_device_bench_result.state_resumed = g_snes_state_resumed;
  snes_device_bench_result.state_refused = g_snes_state_refuse[0];
  snes_device_bench_result.initial_guest_frame = snes->frames;
  snes_device_bench_result.initial_state_crc32 = snes_device_state_hash();
  snes_device_bench_result.audio_buffer_samples = SNES_AUDIO_SAMPLES;
  snes_device_bench_result.gapfree = g_snes_audio_gapfree;
  snes_device_bench_result.speedup = odroid_system_get_app()->speedupEnabled;
}
static void snes_device_bench_tick(void) {
  uint32_t n = ++snes_device_bench_result.completed;
  if (n == SNES_BENCH_WARMUP) {
    snes_device_bench_result.start_guest_frame = snes->frames;
    snes_device_bench_result.start_emu = g_common_emu_frames;
    snes_device_bench_result.start_drawn = g_common_drawn_frames;
    snes_device_bench_result.start_audio_calls = snes_device_bench_result.audio_calls;
    snes_device_bench_result.start_audio_samples = snes_device_bench_result.audio_samples;
    snes_device_bench_result.start_muted_calls = snes_device_bench_result.muted_calls;
    snes_device_bench_result.start_dma = dma_counter;
    snes_device_bench_result.start_underruns = snes_stretch_underruns();
    common_emu_bench_begin();
    /* Timer starts AFTER the smoke breakpoint; debugger pause is excluded. */
    snes_device_bench_result.start_ms = HAL_GetTick();
  }
  if (n != SNES_BENCH_WARMUP + SNES_BENCH_FRAMES) return;
  /* Everything expensive below is outside the measured interval. */
  snes_device_bench_result.end_ms = HAL_GetTick();
  snes_device_bench_result.end_emu = g_common_emu_frames;
  snes_device_bench_result.end_drawn = g_common_drawn_frames;
  snes_device_bench_result.end_audio_calls = snes_device_bench_result.audio_calls;
  snes_device_bench_result.end_audio_samples = snes_device_bench_result.audio_samples;
  snes_device_bench_result.end_muted_calls = snes_device_bench_result.muted_calls;
  snes_device_bench_result.end_guest_frame = snes->frames;
  snes_device_bench_result.end_dma = dma_counter;
  snes_device_bench_result.end_underruns = snes_stretch_underruns();
  snes_device_bench_result.clock_hz = SystemCoreClock;
  snes_device_bench_result.last_drawn_frame = n - 1;
  snes_device_bench_result.framebuffer = (uint32_t)lcd_get_inactive_buffer();
  snes_device_bench_result.end_state_crc32 = snes_device_state_hash();
  audio_stop_playing();
  snes_device_bench_result.done = 1;
  common_emu_bench_complete();
  for (;;) { wdog_refresh(); __WFI(); }
}
