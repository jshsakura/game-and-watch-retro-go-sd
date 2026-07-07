/* Host unit test for the Clock MP3-alarm module (rg_clock_alarm_mp3.c).
 *
 * Covers the fallback DECISION LADDER (never a silent alarm) and the ring
 * service/stop state — every hardware touchpoint (overlay staging, the streaming
 * decoder, the SAI DMA, cache ops, watchdog) is stubbed here. The overlay SIZE
 * linker symbols come from --defsym in run.sh (see the compile line); the
 * pointer symbols get real backing buffers below. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* /clock/alarm.mp3 -> a real temp file so the fopen probe works without root. */
static const char *test_path(const char *p)
{ static char b[256]; snprintf(b, sizeof b, "/tmp/mtest/m%s", p + 1);
  for (char *c = b + 11; *c; c++) if (*c == '/') *c = '_';   /* flatten subdirs */
  return b; }
#define fopen(p, m) fopen(test_path(p), m)
#include "../Core/Src/retro-go/rg_clock_alarm_mp3.c"
#undef fopen

/* ---- linker-symbol backing (pointer symbols; sizes via --defsym) -------- */
void   *__RAM_EMU_START__[512];
void   *_OVERLAY_MUSIC_BSS_START[512];

/* ---- stubbed firmware / decoder ---------------------------------------- */
void wdog_refresh(void) {}
void SCB_CleanDCache_by_Addr(uint32_t *a, int32_t n) { (void)a; (void)n; }

static size_t stub_cache_ret;                 /* bytes the overlay load "reads" */
size_t odroid_overlay_cache_file_in_ram(const char *path, uint8_t *dst)
{ (void)path; (void)dst; return stub_cache_ret; }

/* decoder model: audio_open success flag, an eof flag, and a ring count. */
static bool stub_open_ok, stub_eof;
static int  stub_ring;
static int  n_open, n_pump, n_seek, n_close, n_start_play, n_stop_play;
static int  n_enable_on, n_enable_off, n_set_calls, last_set_vol, last_set_play;

bool audio_open(const char *path)      { (void)path; n_open++; return stub_open_ok; }
void audio_pump(int target)            { (void)target; n_pump++; }
void audio_seek(float frac)            { (void)frac; n_seek++; stub_eof = false; stub_ring = 4096; }
bool audio_eof(void)                   { return stub_eof; }
int  audio_ring_count(void)            { return stub_ring; }
void audio_close(void)                 { n_close++; }

void audio_start_playing(uint16_t len) { (void)len; n_start_play++; }
void audio_stop_playing(void)          { n_stop_play++; }
void music_audio_enable(int on)        { if (on) n_enable_on++; else n_enable_off++; }
void music_audio_set(int vol, int play){ n_set_calls++; last_set_vol = vol; last_set_play = play; }

/* ---- harness ----------------------------------------------------------- */
static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS  %s\n", name); \
    else { printf("FAIL  %s\n", name); fails++; } } while (0)

/* Counters only — s_active is owned by start()/stop(), never poked directly. */
static void reset_counts(void)
{ n_open = n_pump = n_seek = n_close = n_start_play = n_stop_play = 0;
  n_enable_on = n_enable_off = n_set_calls = last_set_vol = last_set_play = 0; }

/* These run AFTER #undef fopen, so use the real fopen with an explicit test_path
 * (the same flattened /tmp/mtest file the module's macro'd fopen probes). */
static void write_alarm_file(void)
{ FILE *f = fopen(test_path("/clock/alarm.mp3"), "wb"); if (f) { fputs("ID3fake", f); fclose(f); } }
static void remove_alarm_file(void)
{ remove(test_path("/clock/alarm.mp3")); }

static void test_available(void)
{
    remove_alarm_file();
    CHECK(clock_alarm_mp3_available() == false, "available: false when no file");
    write_alarm_file();
    CHECK(clock_alarm_mp3_available() == true,  "available: true when /clock/alarm.mp3 exists");
}

/* The fallback ladder: any missing/undecodable stage -> false (caller beeps). */
static void test_start_fallbacks(void)
{
    write_alarm_file();

    reset_counts(); stub_cache_ret = 0;                  /* core bin missing/short */
    CHECK(clock_alarm_mp3_start() == false && !s_active, "start: false when core bin fails to load");
    CHECK(n_open == 0, "start: no audio_open once the overlay failed to stage");

    reset_counts(); stub_cache_ret = 64; stub_open_ok = false;   /* file unreadable */
    CHECK(clock_alarm_mp3_start() == false && !s_active, "start: false when audio_open fails");

    reset_counts(); stub_cache_ret = 64; stub_open_ok = true;    /* opens but decodes nothing */
    stub_eof = true; stub_ring = 0;
    CHECK(clock_alarm_mp3_start() == false && !s_active, "start: false when nothing decodes (eof+empty)");
    CHECK(n_close == 1, "start: closes the file on an undecodable open");
    CHECK(n_start_play == 0 && n_enable_on == 0, "start: never grabs the SAI on failure");
}

static void test_start_success(void)
{
    write_alarm_file();
    reset_counts(); stub_cache_ret = 64; stub_open_ok = true; stub_eof = false; stub_ring = 4096;
    CHECK(clock_alarm_mp3_start() == true && s_active, "start: true when audio primes the ring");
    CHECK(n_start_play == 1, "start: (re)starts the SAI DMA");
    CHECK(n_enable_on == 1, "start: hands the DMA ring to the ISR");
    CHECK(clock_alarm_mp3_active() == true, "active: true after a successful start");
}

/* service: refill while playing, loop at end, and push the current volume. */
static void test_service(void)
{
    write_alarm_file();
    reset_counts(); stub_cache_ret = 64; stub_open_ok = true; stub_eof = false; stub_ring = 4096;
    clock_alarm_mp3_start();

    reset_counts();
    stub_eof = false;                                   /* mid-stream */
    clock_alarm_mp3_service(200);
    CHECK(n_pump == 1 && n_seek == 0, "service: pumps while decoding");
    CHECK(n_set_calls == 1 && last_set_vol == 200 && last_set_play == 1, "service: applies the given volume");

    reset_counts();
    stub_eof = true; stub_ring = 0;                     /* reached the end, ring drained */
    clock_alarm_mp3_service(128);
    CHECK(n_seek == 1, "service: loops (seek to 0) at end of file");

    reset_counts();
    stub_eof = true; stub_ring = 500;                   /* eof but tail still in the ring */
    clock_alarm_mp3_service(128);
    CHECK(n_seek == 0 && n_pump == 0, "service: waits out the ring tail before looping");

    /* service on an inactive engine is inert (no crash, no calls) */
    clock_alarm_mp3_stop();
    reset_counts();
    clock_alarm_mp3_service(128);
    CHECK(n_pump == 0 && n_seek == 0 && n_set_calls == 0, "service: no-op when not active");
}

static void test_stop(void)
{
    write_alarm_file();
    reset_counts(); stub_cache_ret = 64; stub_open_ok = true; stub_eof = false; stub_ring = 4096;
    clock_alarm_mp3_start();

    reset_counts();
    clock_alarm_mp3_stop();
    CHECK(n_enable_off == 1 && n_stop_play == 1 && n_close == 1, "stop: releases ISR, SAI and file");
    CHECK(!s_active && !clock_alarm_mp3_active(), "stop: clears active");

    reset_counts();
    clock_alarm_mp3_stop();                             /* idempotent */
    CHECK(n_enable_off == 0 && n_stop_play == 0 && n_close == 0, "stop: idempotent when already stopped");
}

int main(void)
{
    test_available();
    test_start_fallbacks();
    test_start_success();
    test_service();
    test_stop();
    printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
