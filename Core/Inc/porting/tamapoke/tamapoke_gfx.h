/* Arduino_GFX-shaped drawing shim over the Game & Watch framebuffer.
 *
 * The upstream game draws through an Arduino_Canvas: a persistent 466x466
 * RGB565 buffer that it flushes to the panel. It redraws incrementally and
 * relies on the canvas keeping what was drawn last frame (galleryDirty and
 * friends; drawGameScene deliberately skips fillScreen because it covers the
 * whole canvas itself).
 *
 * The method names and signatures below match Arduino_GFX exactly, so the
 * ported UI keeps its call sites unchanged and the port becomes a question of
 * coordinates rather than of API. Only the 14 calls the game actually uses are
 * provided -- this is a shim, not a graphics library.
 *
 * Canvas semantics are preserved by drawing into the active framebuffer and
 * never swapping; flush() pushes it out. Double buffering would break the
 * incremental redraws, since each swap would expose the frame before last.
 */
#pragma once

#include <stdint.h>

#define GFX_WIDTH 320
#define GFX_HEIGHT 240

/* Glyph cell, scaled by setTextSize(). We reuse the launcher's own
 * font8x8_basic rather than vendoring Adafruit's 5x7, which means the cell is
 * 8x8 and not 6x8: text is 33% wider than upstream at the same setTextSize().
 * Layout work has to budget for that -- it is the usual reason a ported label
 * runs off the end of its button. */
#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8

class Gfx {
 public:
  void begin();
  void flush();

  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                    uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

  void setCursor(int16_t x, int16_t y);
  void setTextColor(uint16_t color);
  void setTextSize(uint8_t size);
  void print(const char *s);
  void print(int v);
  void printf(const char *fmt, ...);

  /* Raw span write, used by the sprite blitters. Bypasses the text state. */
  void writePixels(int16_t x, int16_t y, const uint16_t *px, int16_t count);

 private:
  int16_t cursor_x_ = 0;
  int16_t cursor_y_ = 0;
  uint16_t text_color_ = 0xFFFF;
  uint8_t text_size_ = 1;
};

extern Gfx *gfx;

/* Clipping is applied inside every primitive: the upstream layout is being
 * re-cut from a 466x466 circle to a 320x240 rectangle, and a half-finished
 * relayout that writes one pixel past the framebuffer corrupts whatever the
 * allocator handed out next rather than drawing wrong. Clip, do not trust. */

/* Draws non-Latin text via the launcher's resident i18n renderer and returns
 * its width. Defined in tamapoke_unicode.cpp, which the host harness swaps out
 * because rg_i18n is firmware-only. */
int tamapoke_draw_unicode(int16_t x, int16_t y, const char *s, uint16_t color);

/* Width of a non-Latin string, from the same renderer that draws it. Layout
 * code must ask rather than estimate: a guess that happens to match on the
 * host is still a guess on the device, and centring built on it drifts. */
int tamapoke_unicode_width(const char *s);
int tamapoke_unicode_height(void);
