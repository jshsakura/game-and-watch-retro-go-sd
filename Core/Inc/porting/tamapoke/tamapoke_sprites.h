/* TPK2 sprite packs, loaded from the SD card into fixed slots.
 *
 * Upstream keeps these in PSRAM through ps_malloc, which is why the packs were
 * never sized: an 8 MB part hides the question. We have 724 KB of RAM_EMU for
 * the whole core, so the packs are regenerated at half scale for this panel and
 * the storage is static.
 *
 * Measured over all 151 species after rescaling: median 31 KB, worst 123,983 B
 * (#095 Onix, #130 Gyarados). PMD_BLOB_MAX is that worst case rounded up. Three
 * slots cover the three packs that can be live at once -- the pet, the previous
 * form during the evolution flash, and the gallery detail view.
 *
 * Static rather than malloc'd on purpose: the sizes are known, there is no
 * fragmentation to lose to, and an overflow becomes a link-time ASSERT in the
 * overlay instead of a null return on the device.
 *
 * The struct names match upstream so the ported UI uses them unchanged.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Actions in a pack. Order is upstream's and is baked into the files. */
enum : uint8_t {
  PMD_IDLE = 0, PMD_WALKL, PMD_WALKR, PMD_SLEEP, PMD_EAT, PMD_HURT,
  PMD_ATTACK, PMD_POSE, PMD_HOP, PMD_NOD, PMD_BREATH, PMD_SIT,
  PMD_NACTS
};

#define PMD_MAX_FRAMES 24
#define PMD_BLOB_MAX (124 * 1024) /* >= 123,983, the measured worst case */
#define PMD_SLOTS 3               /* pet + evolving-from + gallery detail */

struct PmdAct {
  uint8_t w = 0, h = 0, frames = 0;
  uint8_t base = 0; /* lowest non-empty row + 1: anchor by the feet */
  uint16_t ms[PMD_MAX_FRAMES];
  const uint8_t *data = nullptr;
};

/* Matches TRANSPARENT in tools/tamapoke/repack_tpk2.py. The packer and the blit
 * have to agree on this number; they did not, and the mismatch drew a black box
 * around every pack sprite. */
#define PMD_TRANSPARENT_INDEX 0xFF

struct PmdMon {
  bool loaded = false;
  uint16_t palCount = 0;
  uint16_t pal[256];
  uint8_t *blob = nullptr;
  PmdAct acts[PMD_NACTS];

  /* Claims a free slot on load and releases it on unload, so the caller keeps
   * upstream's plain "pmd.load(dex, shiny)" and never sees the pool. */
  bool load(uint8_t dexNum, bool shiny = false);
  void unload();
  bool has(uint8_t a) const { return loaded && a < PMD_NACTS && acts[a].frames > 0; }

 private:
  int8_t slot_ = -1;
};

/* Gallery thumbnails: the whole file, resident. Regenerated at 24x24 for this
 * panel, which takes it from 169 KB to 54 KB. */
#define THUMBS_MAX (56 * 1024)

struct SdThumbs {
  bool loaded = false;
  uint8_t *data = nullptr;
  uint16_t count = 0;

  bool load();
  const uint8_t *get(int16_t dex) const;
};

extern SdThumbs thumbs;
extern bool sdReady;
