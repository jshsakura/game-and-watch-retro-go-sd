/* The flash-fallback sprites, loaded from the card.
 *
 * Upstream keeps nine 32x32 character maps of the starter lines in species.h.
 * They are used by the starter picker and the evolution flash, and they are
 * depictions of trademarked characters -- so they were taken out of the source
 * and now arrive with the assets, exactly as the species names do. What is
 * left in the firmware is an egg, a turd and a heart, which belong to nobody.
 *
 * Every row starts pointing at a blank line. A card without sprites9.bin draws
 * empty sprites instead of dereferencing null, which is the difference between
 * a missing optional asset and a crash.
 */
#include "species.h"

#include <string.h>

#include "tamapoke_assets.h"

#define SPRITE_BYTES (FALLBACK_SPRITE_COUNT * FALLBACK_SPRITE_DIM * FALLBACK_SPRITE_DIM)
#define HEADER_BYTES 7 /* "TSPR" + count + w + h */

static const char BLANK_ROW[FALLBACK_SPRITE_DIM + 1] =
    "................................";

/* Rows are NUL-terminated in place, so the blob carries one extra byte per row
 * beyond the packed 32. */
static char g_rows[FALLBACK_SPRITE_COUNT][FALLBACK_SPRITE_DIM][FALLBACK_SPRITE_DIM + 1];
static uint8_t g_raw[HEADER_BYTES + SPRITE_BYTES];

const char *SPR_FALLBACK[FALLBACK_SPRITE_COUNT][FALLBACK_SPRITE_DIM] = {};

static void point_all_at_blank(void) {
  for (int s = 0; s < FALLBACK_SPRITE_COUNT; s++)
    for (int r = 0; r < FALLBACK_SPRITE_DIM; r++) SPR_FALLBACK[s][r] = BLANK_ROW;
}

bool tamapoke_load_fallback_sprites(void) {
  point_all_at_blank();

  uint32_t got = tamapoke_assets_read("sprites9.bin", g_raw, sizeof(g_raw));
  if (got < HEADER_BYTES || memcmp(g_raw, "TSPR", 4) != 0) return false;

  uint8_t count = g_raw[4], w = g_raw[5], h = g_raw[6];
  if (count > FALLBACK_SPRITE_COUNT || w != FALLBACK_SPRITE_DIM ||
      h != FALLBACK_SPRITE_DIM ||
      got < (uint32_t)HEADER_BYTES + (uint32_t)count * w * h) {
    return false;
  }

  const uint8_t *p = g_raw + HEADER_BYTES;
  for (uint8_t s = 0; s < count; s++) {
    for (uint8_t r = 0; r < h; r++) {
      memcpy(g_rows[s][r], p, w);
      g_rows[s][r][w] = '\0';
      SPR_FALLBACK[s][r] = g_rows[s][r];
      p += w;
    }
  }
  return true;
}
