/* Arduino surface + persistence for the ported game. See tamapoke_shim.h. */
#include "tamapoke_shim.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <Preferences.h>

#include "rtcbat.h"

extern "C" {
#include "bq24072.h"
#include "rg_rtc.h"
}

/* An unset STM32 RTC reads back somewhere in the year 2000, and the pet ages by
 * diffing epochs, so anything before this floor is treated as "no clock" rather
 * than as a real date. Without the gate, a first run on a fresh unit hands the
 * pet the whole two-week offline catch-up at once. */
#define CLOCK_SANE_FLOOR_EPOCH 1609459200u /* 2021-01-01 UTC */

#define SAVE_PATH "/tamapoke.sav"

SerialShim Serial;

static uint64_t g_boot_ms;

void tamapoke_shim_init(void) { g_boot_ms = GW_GetCurrentMillis(); }

uint32_t tamapoke_millis(void) {
  return (uint32_t)(GW_GetCurrentMillis() - g_boot_ms);
}

uint32_t tamapoke_epoch(void) { return (uint32_t)GW_GetUnixTime(); }

bool tamapoke_clock_is_set(void) {
  return tamapoke_epoch() >= CLOCK_SANE_FLOOR_EPOCH;
}

void tamapoke_local_time(int *hour, int *minute) {
  struct tm tm;
  GW_GetUnixTM(&tm);
  if (hour) *hour = tm.tm_hour;
  if (minute) *minute = tm.tm_min;
}

/* Filtered: the raw reading swings with load, and the pet's status bar is on
 * screen permanently, so an unfiltered percentage visibly jitters. */
int tamapoke_bat_percent(void) { return bq24072_get_percent_filtered(); }

bool tamapoke_bat_charging(void) { return bq24072_get_state() == BQ24072_STATE_CHARGING; }

/* Upstream calls delay() from a handful of setup paths only; the play loop is
 * paced by the frame loop, so busy-waiting here costs nothing that matters. */
void delay(uint32_t ms) {
  uint32_t until = tamapoke_millis() + ms;
  while ((int32_t)(tamapoke_millis() - until) < 0) {
  }
}

/* ---------------------------------------------------------------- */
/* Save blob. One file, written only when the caller says it is safe. */
/* ---------------------------------------------------------------- */

bool tamapoke_save_write(const void *data, size_t len) {
  if (!data || len == 0) return false;
  FILE *f = fopen(SAVE_PATH, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

bool tamapoke_save_read(void *data, size_t len) {
  if (!data || len == 0) return false;
  FILE *f = fopen(SAVE_PATH, "rb");
  if (!f) return false;
  bool ok = fread(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

/* ---------------------------------------------------------------- */
/* Preferences: one namespace, held in RAM, flushed on demand.       */
/* ---------------------------------------------------------------- */

/* Upstream opens the same "tamapoke" namespace from two places (Pet::prefs and
 * i18n's local handle), so the table is global rather than per-instance --
 * which is also what NVS namespaces actually mean. */
#define PREFS_MAGIC 0x504D4154u /* 'TAMP' */

typedef struct {
  uint32_t magic;
  uint16_t count;
} prefs_header_t;

static Preferences::Entry g_entries[PREFS_MAX_ENTRIES];
static uint16_t g_count;
static bool g_loaded;
static bool g_dirty;

static void prefs_load(void) {
  if (g_loaded) return;
  g_loaded = true;

  FILE *f = fopen(SAVE_PATH, "rb");
  if (!f) return;

  prefs_header_t h = {0, 0};
  if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == PREFS_MAGIC &&
      h.count <= PREFS_MAX_ENTRIES) {
    g_count = (uint16_t)fread(g_entries, sizeof(g_entries[0]), h.count, f);
  }
  fclose(f);
}

bool tamapoke_prefs_commit(void) {
  if (!g_dirty) return true;

  FILE *f = fopen(SAVE_PATH, "wb");
  if (!f) return false;

  prefs_header_t h = {PREFS_MAGIC, g_count};
  bool ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
            fwrite(g_entries, sizeof(g_entries[0]), g_count, f) == g_count;
  fclose(f);
  if (ok) g_dirty = false;
  return ok;
}

bool Preferences::begin(const char *name, bool readOnly) {
  (void)name; /* single namespace */
  read_only_ = readOnly;
  prefs_load();
  return true;
}

void Preferences::end() {
  if (!read_only_) tamapoke_prefs_commit();
}

bool Preferences::clear() {
  g_count = 0;
  g_dirty = true;
  return true;
}

Preferences::Entry *Preferences::find(const char *key) {
  for (uint16_t i = 0; i < g_count; i++)
    if (strncmp(g_entries[i].key, key, PREFS_MAX_KEY) == 0) return &g_entries[i];
  return NULL;
}

bool Preferences::isKey(const char *key) { return find(key) != NULL; }

size_t Preferences::put(const char *key, const void *v, size_t len) {
  if (read_only_ || !key || !v || len == 0 || len > PREFS_MAX_VAL) return 0;

  Entry *e = find(key);
  if (!e) {
    if (g_count >= PREFS_MAX_ENTRIES) return 0;
    e = &g_entries[g_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, PREFS_MAX_KEY - 1);
  }
  e->len = (uint8_t)len;
  memcpy(e->val, v, len);
  g_dirty = true; /* stays in RAM until commit -- see Preferences.h */
  return len;
}

size_t Preferences::getBytes(const char *key, void *out, size_t len) {
  Entry *e = find(key);
  if (!e || !out) return 0;
  size_t n = e->len < len ? e->len : len;
  memcpy(out, e->val, n);
  return n;
}

size_t Preferences::getString(const char *key, char *out, size_t len) {
  Entry *e = find(key);
  if (!out || len == 0) return 0;
  if (!e) {
    out[0] = '\0';
    return 0;
  }
  size_t n = e->len < len ? e->len : len - 1;
  memcpy(out, e->val, n);
  out[n] = '\0';
  return n;
}
