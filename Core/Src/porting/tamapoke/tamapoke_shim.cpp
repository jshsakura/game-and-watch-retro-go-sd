/* Arduino surface + persistence for the ported game. See tamapoke_shim.h. */
#include "tamapoke_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <Preferences.h>

#include "rtcbat.h"

extern "C" {
#include "bq24072.h"
#include "odroid_sdcard.h"   /* mkdir / unlink */
#include "odroid_system.h"   /* odroid_system_get_path, ODROID_PATH_SAVE_SRAM */
#include "rg_rtc.h"
#include "rom_manager.h"     /* ACTIVE_FILE */
}

/* An unset STM32 RTC reads back somewhere in the year 2000, and the pet ages by
 * diffing epochs, so anything before this floor is treated as "no clock" rather
 * than as a real date. Without the gate, a first run on a fresh unit hands the
 * pet the whole two-week offline catch-up at once. */
#define CLOCK_SANE_FLOOR_EPOCH 1609459200u /* 2021-01-01 UTC */

/* Where the pet lives.
 *
 * There is a rule for this and it is not "pick a filename": every other homebrew
 * in the tree -- Zelda 3, Super Mario World, Super Metroid -- asks the launcher,
 *
 *     odroid_system_get_path(ODROID_PATH_SAVE_SRAM, ACTIVE_FILE->path)
 *
 * which yields ODROID_BASE_PATH_SAVES + the ROM path relative to /roms + ".sram",
 * i.e. /data/homebrew/TamaPoke.bin.sram. That is the right answer twice over: the
 * pet IS cartridge-backed memory rather than a snapshot (which is why the hook
 * that persists it is sram_save), and going through the launcher means the
 * launcher's own save management sees the file.
 *
 * This was "/tamapoke.sav" -- the ROOT of the card, next to the user's own
 * folders, which is exactly where they found it. Nothing was wrong with the file;
 * it was in a directory that belongs to the person holding the card.
 *
 * The old path is still READ, once, and then removed -- see prefs_load(). A pet
 * that has been raised for days is in that file, and "moved the save" must not
 * mean "started a new pet". */
#define SAVE_PATH_LEGACY "/tamapoke.sav"

/* Resolved once per boot: odroid_system_get_path() strdup's, and this is asked for
 * on every commit. ACTIVE_FILE is the launcher's current entry, so the stem is
 * /homebrew/TamaPoke.bin exactly as the launcher would write it. */
static char g_save_path[RG_PATH_MAX];

static const char *save_path(void) {
  if (g_save_path[0]) return g_save_path;
  /* The strdup'ing API, because that is the one the vendored header exposes and
   * the one every other core calls. Copied into a static and freed immediately --
   * this is asked for on every commit and the DTCM heap is shared with the
   * launcher. */
  char *p = odroid_system_get_path(ODROID_PATH_SAVE_SRAM,
                                   ACTIVE_FILE ? ACTIVE_FILE->path : NULL);
  if (p) {
    strncpy(g_save_path, p, sizeof(g_save_path) - 1);
    g_save_path[sizeof(g_save_path) - 1] = '\0';
    free(p);
  } else {
    /* Never leave the path empty: an fopen("") fails silently and the pet would
     * stop persisting with nothing on screen to say so. */
    strncpy(g_save_path, ODROID_BASE_PATH_SAVES "/homebrew/TamaPoke.bin.sram",
            sizeof(g_save_path) - 1);
  }
  return g_save_path;
}

/* The directory the launcher would have made for a savestate. fopen will not
 * create it, and a card that has never held one does not have it. */
static void save_dir_ready(void) {
  static bool done = false;
  if (done) return;
  done = true;
  char dir[RG_PATH_MAX];
  strncpy(dir, save_path(), sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return;
  *slash = '\0';
  odroid_sdcard_mkdir(dir);
}

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

/* tamapoke_save_write()/tamapoke_save_read() used to live here: a second save API,
 * never called by anything, writing a raw blob to THE SAME PATH as the
 * preferences store below. Nothing was broken by them because nothing used them,
 * and the first caller would have overwritten the pet with whatever it was
 * saving. Deleted rather than pointed somewhere else -- there is one save here. */

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

static bool prefs_read_from(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;

  prefs_header_t h = {0, 0};
  bool ok = false;
  if (fread(&h, sizeof(h), 1, f) == 1 && h.magic == PREFS_MAGIC &&
      h.count <= PREFS_MAX_ENTRIES) {
    g_count = (uint16_t)fread(g_entries, sizeof(g_entries[0]), h.count, f);
    ok = (g_count == h.count);
  }
  fclose(f);
  if (!ok) g_count = 0;
  return ok;
}

static void prefs_load(void) {
  if (g_loaded) return;
  g_loaded = true;

  if (prefs_read_from(save_path())) return;

  /* One-shot migration off the card root. Runs during init -- before the play
   * loop, so the write is at a safe point -- and only when the new path has
   * nothing to offer. The legacy file is removed only after the new one is
   * actually on the card, so a failed write leaves the player's pet where it is
   * rather than deleting it and starting over. */
  if (prefs_read_from(SAVE_PATH_LEGACY)) {
    g_dirty = true;
    if (tamapoke_prefs_commit()) odroid_sdcard_unlink(SAVE_PATH_LEGACY);
  }
}

bool tamapoke_prefs_commit(void) {
  if (!g_dirty) return true;

  save_dir_ready();

  FILE *f = fopen(save_path(), "wb");
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
