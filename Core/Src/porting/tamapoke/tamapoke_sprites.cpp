/* TPK2 / TPTH loading. See tamapoke_sprites.h for the sizing rationale.
 *
 * The parsing below is upstream's, kept deliberately intact -- it validates a
 * truncated or corrupt pack at every step, and re-deriving that from scratch
 * would only lose the checks. What changed is the two things that were tied to
 * the ESP32: ps_malloc became a static slot, and SD_MMC became stdio over
 * FatFS.
 */
#include "tamapoke_sprites.h"

#include "tamapoke_assets.h"

#include <stdio.h>
#include <string.h>

#define TPK2_MIN_SIZE 7

bool sdReady = false;
SdThumbs thumbs;

/* Slot pool. These are the core's largest objects by a wide margin; the
 * overlay's linker ASSERT is what proves they still fit. */
static uint8_t g_slot_mem[PMD_SLOTS][PMD_BLOB_MAX];
static bool g_slot_used[PMD_SLOTS];
static uint8_t g_thumbs_mem[THUMBS_MAX];

static int8_t slot_claim(void) {
  for (int8_t i = 0; i < PMD_SLOTS; i++) {
    if (!g_slot_used[i]) {
      g_slot_used[i] = true;
      return i;
    }
  }
  return -1;
}

/* Entries come out of the one .dat on the card rather than loose files; see
 * tamapoke_assets.cpp for why. `cap` is the caller's hard limit -- a pack
 * larger than the slot means the asset pipeline emitted full-size sprites,
 * which is a build mistake, not something to grow for at runtime. */
static uint32_t read_entry(const char *name, uint8_t *dst, uint32_t cap) {
  return tamapoke_assets_read(name, dst, cap);
}

void PmdMon::unload() {
  if (slot_ >= 0) {
    g_slot_used[slot_] = false;
    slot_ = -1;
  }
  loaded = false;
  blob = nullptr;
  palCount = 0;
  memset(acts, 0, sizeof(acts));
}

/* Walk the action table, pointing each PmdAct at its frames inside the blob.
 * Every offset is bounds-checked against `end`; a truncated pack must fail the
 * load rather than hand the renderer a pointer past the buffer. */
static bool parse_actions(PmdMon *m, const uint8_t *p, const uint8_t *end, uint8_t n_acts) {
  for (uint8_t i = 0; i < n_acts && p + 4 <= end; i++) {
    uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
    p += 4;
    if (id >= PMD_NACTS || nf > PMD_MAX_FRAMES) return false;

    uint32_t bytes = (uint32_t)nf * 2 + (uint32_t)w * h * nf;
    if (w == 0 || h == 0 || nf == 0 || p + bytes > end) return false;

    PmdAct &a = m->acts[id];
    a.w = w;
    a.h = h;
    a.frames = nf;
    for (uint8_t k = 0; k < nf; k++) {
      a.ms[k] = (uint16_t)(p[0] | (p[1] << 8));
      p += 2;
    }
    a.data = p;
    p += (uint32_t)w * h * nf;

    /* Lowest row with any pixel, over every frame: the sprite is anchored by
     * the feet, not by its canvas, or it bobs against the ground. */
    uint8_t base = 1;
    for (uint8_t f = 0; f < nf; f++) {
      const uint8_t *fr = a.data + (uint32_t)f * w * h;
      for (uint8_t row = h; row > 0; row--) {
        bool empty = true;
        for (uint8_t col = 0; col < w; col++) {
          if (fr[(row - 1) * w + col] != 0xFF) { empty = false; break; }
        }
        if (!empty) {
          if (row > base) base = row;
          break;
        }
      }
    }
    a.base = base;
  }
  return true;
}

bool PmdMon::load(uint8_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  slot_ = slot_claim();
  if (slot_ < 0) return false;
  uint8_t *dst = g_slot_mem[slot_];

  char name[16];
  snprintf(name, sizeof(name), "p%s%03u.bin", shiny ? "s" : "", dexNum);
  uint32_t size = read_entry(name, dst, PMD_BLOB_MAX);
  if (!size && shiny) { /* no shiny pack: fall back to the normal one */
    snprintf(name, sizeof(name), "p%03u.bin", dexNum);
    size = read_entry(name, dst, PMD_BLOB_MAX);
  }
  if (size < TPK2_MIN_SIZE || memcmp(dst, "TPK2", 4) != 0) {
    unload();
    return false;
  }

  blob = dst;
  uint8_t n_acts = blob[4];
  memcpy(&palCount, blob + 5, 2);
  if (palCount > 256 || (uint32_t)TPK2_MIN_SIZE + palCount * 2 > size) {
    unload();
    return false;
  }
  memcpy(pal, blob + TPK2_MIN_SIZE, palCount * 2);

  const uint8_t *p = blob + TPK2_MIN_SIZE + palCount * 2;
  if (!parse_actions(this, p, blob + size, n_acts)) {
    unload();
    return false;
  }

  loaded = true;
  return true;
}

/* ---------------------------------------------------------------- */

bool SdThumbs::load() {
  if (!sdReady) return false;

  uint32_t n = read_entry("thumbs.bin", g_thumbs_mem, THUMBS_MAX);
  if (n < 6 || memcmp(g_thumbs_mem, "TPTH", 4) != 0) return false;

  data = g_thumbs_mem;
  size = n;
  memcpy(&count, data + 4, 2);
  /* The offset table has to be inside the file before anything reads it. */
  if ((uint32_t)6 + 4u * count > size) {
    loaded = false;
    return false;
  }
  loaded = true;
  return true;
}

bool SdThumbs::get(int16_t dex, SdThumb *out) const {
  if (!out || !loaded || dex < 1 || dex > (int16_t)count) return false;
  uint32_t off;
  memcpy(&off, data + 6 + 4 * (dex - 1), 4);
  if (off + 3 > size) return false;

  SdThumb t;
  t.w = data[off];
  t.h = data[off + 1];
  t.palCount = data[off + 2];
  if (!t.w || !t.h || !t.palCount) return false;

  const uint32_t need = 3u + 2u * t.palCount + (uint32_t)t.w * t.h;
  if (off + need > size) return false;

  t.pal = data + off + 3;
  t.px = t.pal + 2u * t.palCount;
  *out = t;
  return true;
}
