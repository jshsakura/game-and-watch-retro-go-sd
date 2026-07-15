/* Host driver for the SNES savestate cold-resume proof.
 *
 * Runs the REAL app_main_snes() (main_snes.c compiled as-is against the shim
 * headers). The shim owns the frame clock: common_emu_sound_sync() fires once
 * per emulated frame, and at the scripted frame numbers it saves / hashes /
 * longjmps out of app_main's infinite loop.
 *
 * Two-process cold-resume (the only trusted proof — a same-process reload can
 * hide state in warm caches):
 *   mode A: boot cold, run to SAVE_FRAME, save, keep running to END_FRAME,
 *           accumulate fb+audio hash over (SAVE_FRAME, END_FRAME], then dump a
 *           final reference savestate and print H_cont.
 *   mode B: fresh process, boot cold, LOAD the file, run the same span with
 *           the same absolute input schedule, print H_res the same way.
 * PASS = identical H (fb+audio+final-machine-state).
 *
 * Input schedule is a function of the ABSOLUTE frame number so both processes
 * agree: Start is held on frames 40..45, 64..69, ... (the harness tap).
 */
#include "shim/odroid_system.h"
#include <setjmp.h>

/* ---- scripted run ---- */
static long g_frame;           /* frames completed */
static long g_save_frame;      /* A: save after this frame; B: resume origin  */
static long g_end_frame;
static int  g_mode;            /* 0 = A (save), 1 = B (load)                  */
static const char *g_state_path;
static const char *g_final_path;
static jmp_buf g_done;

/* captured from odroid_system_emu_init — the REAL serializer in main_snes.c */
static state_fn_t g_load_fn, g_save_fn;

/* ---- fnv1a running hash over fb+audio for every frame after the save ---- */
static uint64_t g_hash = 1469598103934665603ULL;
static void hash_bytes(const void *p, size_t n) {
  const uint8_t *b = p;
  while (n--) { g_hash ^= *b++; g_hash *= 1099511628211ULL; }
}

/* ---- lcd / audio buffers ---- */
static uint16_t g_fb[320 * 240];
static int16_t  g_audio[1024];
static uint16_t g_audio_len = 266;   /* SNES_AUDIO_SAMPLES */

uint16_t *lcd_get_active_buffer(void) { return g_fb; }
void lcd_swap(void) {}
void lcd_clear_buffers(void) { memset(g_fb, 0, sizeof(g_fb)); }
void lcd_clear_active_buffer(void) { memset(g_fb, 0, sizeof(g_fb)); }
void lcd_wait_for_vblank(void) {}
void lcd_set_refresh_rate(int hz) { (void)hz; }

void audio_start_playing(int spf) { g_audio_len = (uint16_t)spf; }
int16_t *audio_get_active_buffer(void) { return g_audio; }
uint16_t audio_get_buffer_length(void) { return g_audio_len; }

/* ---- input: absolute-frame Start tap ---- */
void odroid_input_read_gamepad(odroid_gamepad_state_t *out) {
  memset(out, 0, sizeof(*out));
  long f = g_frame + 1;                 /* the frame about to run */
  if (f >= 40 && (f % 24) < 6)
    out->values[ODROID_INPUT_START] = 1;
}

/* ---- system shims ---- */
common_emu_state_t common_emu_state;
retro_emulator_file_t *ACTIVE_FILE;

void odroid_system_init(int a, int s) { (void)a; (void)s; }
void odroid_system_emu_init(state_fn_t load, state_fn_t save, screenshot_fn_t ss,
                            void *a, void *b, void *c) {
  (void)ss; (void)a; (void)b; (void)c;
  g_load_fn = load;
  g_save_fn = save;
}
void odroid_system_emu_load_state(int slot) {
  (void)slot;
  bool ok = g_load_fn(g_state_path);
  printf("[shim] load_state(%s) -> %s\n", g_state_path, ok ? "OK" : "REFUSED");
  if (getenv("EXPECT_REFUSE")) {
    /* refusal test: report and stop — the point was the return value + no crash */
    printf("REFUSAL_RESULT %s\n", ok ? "ACCEPTED" : "REFUSED");
    exit(ok ? 1 : 0);
  }
  if (!ok) { printf("FATAL: load refused in mode B\n"); exit(3); }
  if (getenv("IMMEDIATE_RESAVE")) {
    /* serializer-losslessness probe: load -> save with zero frames run.
     * byte-diff vs the original file localizes any non-round-tripping field. */
    g_save_fn(getenv("IMMEDIATE_RESAVE"));
    exit(0);
  }
}
void odroid_system_switch_app(int app) {
  (void)app;
  printf("FATAL: switch_app (unsupported cart?)\n");
  exit(4);
}
void odroid_audio_mute(bool m) { (void)m; }
void odroid_overlay_alert(const char *msg) { printf("[alert] %s\n", msg); }

const uint8_t *odroid_overlay_cache_file_in_flash(const char *path, uint32_t *size, bool exec) {
  (void)exec;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
  fclose(f);
  *size = (uint32_t)n;
  return buf;
}

bool common_emu_frame_loop(void) { return true; }   /* deterministic: draw all */
void common_emu_input_loop(odroid_gamepad_state_t *j, odroid_dialog_choice_t *o,
                           void (*r)(void)) { (void)j; (void)o; (void)r; }
void common_emu_input_loop_handle_turbo(odroid_gamepad_state_t *j) { (void)j; }
bool common_emu_sound_loop_is_muted(void) { return false; }
int32_t common_emu_sound_get_volume(void) { return 256; }
void common_emu_auto_oc(int level) { (void)level; }
void common_ingame_overlay(void) {}
void wdog_refresh(void) {}

void *itc_malloc(size_t s) { return malloc(s); }
void *itc_calloc(size_t n, size_t s) { return calloc(n, s); }
void *ahb_malloc(size_t s) { return malloc(s); }
void *ram_malloc(size_t s) { return malloc(s); }
void *ram_calloc(size_t n, size_t s) { return calloc(n, s); }

/* ---- the frame clock: fires once per emulated frame ---- */
void common_emu_sound_sync(bool late) {
  (void)late;
  g_frame++;

  if (g_frame > g_save_frame) {         /* frames after the resume point */
    hash_bytes(g_fb, sizeof(g_fb));
    hash_bytes(g_audio, g_audio_len * sizeof(int16_t));
  }

  if (g_mode == 0 && g_frame == g_save_frame) {
    bool ok = g_save_fn(g_state_path);
    printf("[shim] save_state(%s) @f%ld -> %s\n", g_state_path, g_frame, ok ? "OK" : "FAIL");
    if (!ok) exit(5);
  }

  if (getenv("TRACE_FRAMES") && g_frame > g_save_frame) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *b = (const uint8_t *)g_fb;
    for (size_t i = 0; i < sizeof(g_fb); i++) { h ^= b[i]; h *= 1099511628211ULL; }
    fprintf(stderr, "f%ld fb=%016llx\n", g_frame, (unsigned long long)h);
  }

  if (g_frame == g_end_frame) {
    /* final machine state via the same serializer, hashed from the file —
     * SEPARATE from the fb+audio hash so a display-only divergence is
     * distinguishable from real machine drift. */
    uint64_t mh = 1469598103934665603ULL;
    if (g_save_fn(g_final_path)) {
      FILE *f = fopen(g_final_path, "rb");
      if (f) {
        uint8_t buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
          for (size_t i = 0; i < n; i++) { mh ^= buf[i]; mh *= 1099511628211ULL; }
        fclose(f);
      }
    }
    printf("RESULT mode=%c frames=%ld..%ld AV=%016llx MACHINE=%016llx\n",
           g_mode ? 'B' : 'A', g_save_frame, g_end_frame,
           (unsigned long long)g_hash, (unsigned long long)mh);
    longjmp(g_done, 1);
  }
}

/* ---- entry ---- */
void app_main_snes(uint8_t load_state, uint8_t start_paused, int8_t save_slot);

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: %s <A|B> <rom> <state-file> <save_frame> <end_frame>\n", argv[0]);
    return 2;
  }
  g_mode = (argv[1][0] == 'B');
  static retro_emulator_file_t file;
  file.path = argv[2];
  ACTIVE_FILE = &file;
  g_state_path = argv[3];
  g_save_frame = atol(argv[4]);
  g_end_frame = atol(argv[5]);
  static char final_path[512];
  snprintf(final_path, sizeof(final_path), "%s.final.%c", argv[3], g_mode ? 'B' : 'A');
  g_final_path = final_path;

  if (g_mode == 1)
    g_frame = g_save_frame;    /* resume origin: input schedule stays absolute */

  if (setjmp(g_done) == 0)
    app_main_snes(g_mode == 1 ? 1 : 0, 0, 0);
  return 0;
}
