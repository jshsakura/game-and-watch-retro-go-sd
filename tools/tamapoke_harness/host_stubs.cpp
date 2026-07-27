/* Hardware layer for the TamaPoke host harness.
 *
 * Only the leaves are faked: the framebuffer, the clock, and the gamepad.
 * Everything above them -- gfx primitives, the focus layer, the UI, the game
 * logic -- is the firmware's own code, compiled from the Makefile's own source
 * list. A harness that reimplements the thing it is testing proves nothing;
 * this tree has paid for that lesson more than once.
 */
#include <stdint.h>
#include <string.h>

#include "tamapoke_shim.h"

extern "C" {
#include "gw_lcd.h"
}
/* Wrapped to match tamapoke_input.cpp: the header has no guard of its own, and
 * the two must agree on linkage or the stub never satisfies the reference. */
extern "C" {
#include "odroid_input.h"
}
#include <Preferences.h>

/* ---------------------------------------------------------------- */
/* Framebuffer                                                       */
/* ---------------------------------------------------------------- */

/* Guard bands either side of the visible area. Nothing should ever write here:
 * gfx clips, so a hit means a primitive bypassed the clip. Sized to catch a
 * plausible overrun rather than every conceivable one. */
#define GUARD_PIXELS 4096
#define GUARD_PATTERN 0x5A5A

static uint16_t g_backing[GUARD_PIXELS + GW_LCD_WIDTH * GW_LCD_HEIGHT + GUARD_PIXELS];
static uint16_t *g_fb = g_backing + GUARD_PIXELS;

extern "C" {
void *lcd_get_active_buffer(void) { return g_fb; }
void *lcd_get_inactive_buffer(void) { return g_fb; }
int lcd_get_mode(void) { return LCD_MODE_RGB565; }
uint16_t lcd_pack_color(uint16_t color) { return color; }
void lcd_swap(void) {}
void lcd_clone(void) {}
void lcd_wait_for_vblank(void) {}
} /* extern "C" */

void harness_fb_reset(uint16_t fill) {
  for (size_t i = 0; i < GUARD_PIXELS; i++) {
    g_backing[i] = GUARD_PATTERN;
    g_backing[GUARD_PIXELS + GW_LCD_WIDTH * GW_LCD_HEIGHT + i] = GUARD_PATTERN;
  }
  for (int i = 0; i < GW_LCD_WIDTH * GW_LCD_HEIGHT; i++) g_fb[i] = fill;
}

/* Non-zero = something drew outside the panel. Returns how many guard words
 * were clobbered so a caller can report scale, not just a boolean. */
int harness_fb_guard_violations(void) {
  int n = 0;
  for (size_t i = 0; i < GUARD_PIXELS; i++) {
    if (g_backing[i] != GUARD_PATTERN) n++;
    if (g_backing[GUARD_PIXELS + GW_LCD_WIDTH * GW_LCD_HEIGHT + i] != GUARD_PATTERN) n++;
  }
  return n;
}

const uint16_t *harness_fb(void) { return g_fb; }

/* ---------------------------------------------------------------- */
/* Gamepad: scripted rather than polled                              */
/* ---------------------------------------------------------------- */

static odroid_gamepad_state_t g_pad;

extern "C" void odroid_input_read_gamepad(odroid_gamepad_state_t *out) { *out = g_pad; }

/* ODROID_INPUT_ANY is past ODROID_INPUT_MAX -- a sentinel, not an index. It
 * has no slot in values[], so there is nothing to maintain here. */
void harness_pad_set(int key, bool down) {
  if (key >= 0 && key < ODROID_INPUT_MAX) g_pad.values[key] = down ? 1 : 0;
}

void harness_pad_clear(void) { memset(&g_pad, 0, sizeof(g_pad)); }

/* ---------------------------------------------------------------- */
/* Clock                                                             */
/* ---------------------------------------------------------------- */

/* Fixed epoch by default so renders are reproducible: the UI draws a real
 * clock, day/night scene tinting and battery state, and a wall clock would
 * make every capture differ from the last. Tests that care about aging move
 * it explicitly. */
static uint64_t g_now_ms = 1753600000000ull; /* 2025-07-27 ~12:26 UTC */

extern "C" {
uint64_t GW_GetCurrentMillis(void) { return g_now_ms; }
long GW_GetUnixTime(void) { return (long)(g_now_ms / 1000); }
}

/* shim.cpp wrappers (skipped here) -- the host only needs to provide the
 * millis/epoch the UI loops call, plus the "clock set" gate sceneHour reads. */
extern "C" uint32_t tamapoke_millis(void)      { return (uint32_t)g_now_ms; }
extern "C" uint32_t tamapoke_epoch(void)       { return (uint32_t)(g_now_ms / 1000); }
extern "C" bool    tamapoke_clock_is_set(void) { return true; }

void harness_clock_set_ms(uint64_t ms) { g_now_ms = ms; }
uint64_t harness_clock_ms(void) { return g_now_ms; }
void harness_clock_advance_ms(uint64_t ms) { g_now_ms += ms; }

/* ---------------------------------------------------------------- */
/* Serial + Preferences (skipped shim.cpp provides these on device)  */
/* ---------------------------------------------------------------- */

SerialShim Serial;

/* Layout tests don't persist: providePreferences::put/find/getBytes as
 * minimal in-memory stubs so pet.cpp links. If a test ever needs to verify
 * save/load round-trip it should link the real tamapoke_shim.cpp instead. */
Preferences::Entry *Preferences::find(const char *) { return nullptr; }
size_t Preferences::put(const char *, const void *, size_t len) { return len; }
size_t Preferences::getBytes(const char *, void *, size_t len) { return 0; }
size_t Preferences::getString(const char *, char *out, size_t) { if (out) out[0] = 0; return 0; }
bool Preferences::isKey(const char *) { return false; }
bool Preferences::begin(const char *, bool) { return true; }
void Preferences::end() {}
bool Preferences::clear() { return true; }

/* rg_i18n is resident firmware and is not in a host build. The harness only
 * needs the layout to be honest about how much room Korean takes, so this
 * draws a filled cell per syllable rather than nothing: a blank would make an
 * overflowing string look like it fits. */
#include "tamapoke_gfx.h"

int tamapoke_unicode_width(const char *s) {
  int cells = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if ((*p & 0xC0) != 0x80) cells++;  /* count UTF-8 lead bytes */
  return cells * GFX_GLYPH_W * 2;      /* CJK is roughly double-width */
}

int tamapoke_unicode_height(void) { return GFX_GLYPH_H * 2; }

int tamapoke_draw_unicode(int16_t x, int16_t y, const char *s, uint16_t color) {
  int w = tamapoke_unicode_width(s);
  gfx->fillRect(x, y, w, tamapoke_unicode_height(), color);
  return w;
}
