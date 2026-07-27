/* Non-Latin text, drawn by the launcher's own i18n renderer.
 *
 * Kept in its own translation unit because the harness replaces exactly this
 * one function: rg_i18n is resident firmware and is not linked into a host
 * build, so a stub stands in there while the device gets the real thing.
 */
#include "tamapoke_gfx.h"

extern "C" {
#include "rg_i18n.h"
}

/* Returns the advance in pixels so the caller can keep its cursor. Width is
 * asked for rather than assumed: a Hangul syllable is not a fixed multiple of
 * the Latin cell. */
int tamapoke_draw_unicode(int16_t x, int16_t y, const char *s, uint16_t color) {
  int w = i18n_get_text_width(s);
  if (x >= GFX_WIDTH || y >= GFX_HEIGHT) return w;

  /* transparent = 1: the UI draws text over a scene it has already rendered,
   * so a background fill would punch a hole in it. */
  i18n_draw_text_line((uint16_t)x, (uint16_t)y, (uint16_t)w, s, color, 0, 1);
  return w;
}
