/* NVS-shaped key/value store backed by one file on the SD card.
 *
 * The vendored game logic saves through the ESP32's Preferences class. Rather
 * than rewrite ~70 call sites, this keeps the same method names and moves the
 * backend: values live in RAM and are flushed as a single blob.
 *
 * ★ put*() never touches the card. A write in the middle of the play loop is
 * how the FAT gets corrupted, and the pet saves constantly. Persistence
 * happens only when the port calls commit() at a safe point -- menu, sleep,
 * exit -- or when end() is called on a writable handle.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PREFS_MAX_ENTRIES 64
#define PREFS_MAX_KEY 12
#define PREFS_MAX_VAL 32 /* largest real value is the 19-byte dex bitmap */

class Preferences {
 public:
  bool begin(const char *name, bool readOnly = false);
  void end();
  bool clear();
  bool isKey(const char *key);

  size_t putBool(const char *key, bool v) { return put(key, &v, sizeof(v)); }
  size_t putChar(const char *key, int8_t v) { return put(key, &v, sizeof(v)); }
  size_t putUChar(const char *key, uint8_t v) { return put(key, &v, sizeof(v)); }
  size_t putShort(const char *key, int16_t v) { return put(key, &v, sizeof(v)); }
  size_t putUShort(const char *key, uint16_t v) { return put(key, &v, sizeof(v)); }
  size_t putInt(const char *key, int32_t v) { return put(key, &v, sizeof(v)); }
  size_t putUInt(const char *key, uint32_t v) { return put(key, &v, sizeof(v)); }
  size_t putBytes(const char *key, const void *v, size_t len) { return put(key, v, len); }
  size_t putString(const char *key, const char *v) { return put(key, v, strlen(v) + 1); }

  bool getBool(const char *key, bool d = false) { return get<bool>(key, d); }
  int8_t getChar(const char *key, int8_t d = 0) { return get<int8_t>(key, d); }
  uint8_t getUChar(const char *key, uint8_t d = 0) { return get<uint8_t>(key, d); }
  int16_t getShort(const char *key, int16_t d = 0) { return get<int16_t>(key, d); }
  uint16_t getUShort(const char *key, uint16_t d = 0) { return get<uint16_t>(key, d); }
  int32_t getInt(const char *key, int32_t d = 0) { return get<int32_t>(key, d); }
  uint32_t getUInt(const char *key, uint32_t d = 0) { return get<uint32_t>(key, d); }
  size_t getBytes(const char *key, void *out, size_t len);
  size_t getString(const char *key, char *out, size_t len);

  /* Public because the backing table lives in the .cpp: the store is one
   * namespace shared by every handle, exactly as NVS namespaces behave. */
  struct Entry {
    char key[PREFS_MAX_KEY];
    uint8_t len;
    uint8_t val[PREFS_MAX_VAL];
  };

 private:
  size_t put(const char *key, const void *v, size_t len);
  Entry *find(const char *key);

  template <typename T>
  T get(const char *key, T fallback) {
    Entry *e = find(key);
    if (!e || e->len != sizeof(T)) return fallback;
    T v;
    memcpy(&v, e->val, sizeof(T));
    return v;
  }

  bool read_only_ = false;
};

/* Flush the store to the card. Call from safe points only. */
bool tamapoke_prefs_commit(void);
