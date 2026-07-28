/* The Arduino surface the ported game still expects.
 *
 * Upstream is an .ino built against the ESP32 Arduino core. Only a small part
 * of that core is actually reachable from the game: a millisecond clock (69
 * call sites), a couple of delays, Serial logging, and NVS-backed Preferences
 * for the save. Everything else it touched -- the panel driver, the touch
 * driver, the ES8311 codec, FreeRTOS tasks -- is replaced wholesale elsewhere
 * in this directory rather than shimmed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds since this app started.
 *
 * Deliberately NOT the raw epoch clock. GW_GetCurrentMillis() counts from 1970,
 * so truncating it to 32 bits would make millis() jump by days across a power
 * cycle, and the game compares it against stored timestamps in ~69 places
 * ("millis() - lastPoll < 20"). Rebasing to app start keeps those differences
 * meaning what upstream meant. Wall-clock aging uses tamapoke_epoch() instead,
 * which is a separate question and wants the real date. */
uint32_t tamapoke_millis(void);

/* Seconds since 1970, straight off the RTC, for the offline-aging maths. */
uint32_t tamapoke_epoch(void);

/* True once the user has actually set the clock. The pet ages while powered
 * off by diffing epochs, so an unset RTC would either freeze it or -- worse --
 * hand it the full two-week catch-up on first run. Callers must gate on this
 * rather than trusting a plausible-looking epoch. */
bool tamapoke_clock_is_set(void);

void tamapoke_shim_init(void);

#ifdef __cplusplus
}

/* Names the ported .ino code uses verbatim. Kept as thin inlines so the call
 * sites do not have to be touched. */
static inline uint32_t millis() { return tamapoke_millis(); }
void delay(uint32_t ms);

/* Serial logging was a development aid on a board with a USB console; here it
 * compiles away. Kept as a type so the ~75 call sites survive untouched. */
struct SerialShim {
  void begin(unsigned long) {}
  void println(const char *) {}
  void println() {}
  void print(const char *) {}
  void printf(const char *, ...) {}
};
extern SerialShim Serial;

/* Persistence. Upstream keeps the save in NVS through Preferences; we keep it in
 * one blob at /data/tamapoke.sav. The play loop must not write -- a mid-play FAT
 * write is how the card gets corrupted -- so commits are gated to the launcher's
 * safe points by the caller (tamapoke_prefs_commit(), declared in Preferences.h),
 * not by this layer.
 *
 * There used to be a tamapoke_save_write()/tamapoke_save_read() pair here as
 * well: a second save API, called by nothing, aimed at the same file as the
 * preferences store. The first caller would have overwritten the pet. */

#endif /* __cplusplus */
