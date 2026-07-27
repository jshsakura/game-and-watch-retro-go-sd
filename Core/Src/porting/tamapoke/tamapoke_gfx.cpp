/* Arduino_GFX-shaped primitives over the Game & Watch framebuffer.
 *
 * See tamapoke_gfx.h for why the signatures mirror Arduino_GFX. Pixels go out
 * through lcd_pen_*, not through raw uint16 stores, so the shim stays correct
 * if the display is running in LUT8 rather than RGB565.
 *
 * Everything clips. The upstream layout is being re-cut from a 466x466 circle
 * to a 320x240 rectangle, so during that work primitives *will* be handed
 * coordinates that fall outside the panel; clipping turns those into a visual
 * bug instead of a heap corruption two allocations away.
 */
#include "tamapoke_gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "gw_lcd.h"
}
#include "bitmaps/font_basic.h"

#define TEXT_BUF_MAX 128

static Gfx g_gfx;
Gfx *gfx = &g_gfx;

/* ---------------------------------------------------------------- */
/* Span primitive. Every filled shape below decomposes into these.   */
/* ---------------------------------------------------------------- */

static inline void span(const lcd_pen_t *p, int y, int x0, int x1) {
  if (y < 0 || y >= GFX_HEIGHT) return;
  if (x0 < 0) x0 = 0;
  if (x1 > GFX_WIDTH - 1) x1 = GFX_WIDTH - 1;
  if (x0 > x1) return;
  lcd_pen_run(p, y * GFX_WIDTH + x0, x1 - x0 + 1);
}

static inline void dot(const lcd_pen_t *p, int x, int y) {
  if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
  lcd_pen_set(p, y * GFX_WIDTH + x);
}

void Gfx::begin() {
  cursor_x_ = cursor_y_ = 0;
  text_color_ = 0xFFFF;
  text_size_ = 1;
}

/* Canvas semantics: we draw into the live buffer and never swap, so there is
 * nothing to present. Kept so upstream call sites survive untouched. */
void Gfx::flush() {}

void Gfx::fillScreen(uint16_t color) { fillRect(0, 0, GFX_WIDTH, GFX_HEIGHT, color); }

void Gfx::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  lcd_pen_t p = lcd_pen(color);
  for (int row = y; row < y + h; row++) span(&p, row, x, x + w - 1);
}

void Gfx::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  lcd_pen_t p = lcd_pen(color);
  int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  int dy = y1 > y0 ? y1 - y0 : y0 - y1;
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  for (;;) {
    dot(&p, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx)  { err += dx; y0 += sy; }
  }
}

/* ---------------------------------------------------------------- */
/* Circles: midpoint walk, emitted as spans so fills stay one pass.  */
/* ---------------------------------------------------------------- */

typedef void (*arc_emit_t)(const lcd_pen_t *p, int cx, int cy, int x, int y);

static void arc_fill(const lcd_pen_t *p, int cx, int cy, int x, int y) {
  span(p, cy + y, cx - x, cx + x);
  span(p, cy - y, cx - x, cx + x);
  span(p, cy + x, cx - y, cx + y);
  span(p, cy - x, cx - y, cx + y);
}

static void arc_outline(const lcd_pen_t *p, int cx, int cy, int x, int y) {
  dot(p, cx + x, cy + y); dot(p, cx - x, cy + y);
  dot(p, cx + x, cy - y); dot(p, cx - x, cy - y);
  dot(p, cx + y, cy + x); dot(p, cx - y, cy + x);
  dot(p, cx + y, cy - x); dot(p, cx - y, cy - x);
}

static void walk_circle(const lcd_pen_t *p, int cx, int cy, int r, arc_emit_t emit) {
  if (r < 0) return;
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    emit(p, cx, cy, x, y);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

void Gfx::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  lcd_pen_t p = lcd_pen(color);
  walk_circle(&p, x, y, r, arc_fill);
}

void Gfx::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  lcd_pen_t p = lcd_pen(color);
  walk_circle(&p, x, y, r, arc_outline);
}

/* ---------------------------------------------------------------- */
/* Rounded rectangles                                                */
/* ---------------------------------------------------------------- */

static int16_t clamp_radius(int16_t w, int16_t h, int16_t r) {
  int16_t limit = (w < h ? w : h) / 2;
  if (r > limit) r = limit;
  return r < 0 ? 0 : r;
}

void Gfx::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = clamp_radius(w, h, r);
  fillRect(x, y + r, w, h - 2 * r, color);

  lcd_pen_t p = lcd_pen(color);
  int cxl = x + r, cxr = x + w - r - 1;
  int cyt = y + r, cyb = y + h - r - 1;
  int cx = r, cy = 0, err = 1 - r;
  while (cx >= cy) {
    span(&p, cyt - cy, cxl - cx, cxr + cx);
    span(&p, cyt - cx, cxl - cy, cxr + cy);
    span(&p, cyb + cy, cxl - cx, cxr + cx);
    span(&p, cyb + cx, cxl - cy, cxr + cy);
    cy++;
    if (err < 0) {
      err += 2 * cy + 1;
    } else {
      cx--;
      err += 2 * (cy - cx) + 1;
    }
  }
}

void Gfx::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  r = clamp_radius(w, h, r);
  lcd_pen_t p = lcd_pen(color);

  span(&p, y, x + r, x + w - r - 1);
  span(&p, y + h - 1, x + r, x + w - r - 1);
  for (int row = y + r; row <= y + h - r - 1; row++) {
    dot(&p, x, row);
    dot(&p, x + w - 1, row);
  }

  int cxl = x + r, cxr = x + w - r - 1;
  int cyt = y + r, cyb = y + h - r - 1;
  int cx = r, cy = 0, err = 1 - r;
  while (cx >= cy) {
    dot(&p, cxr + cx, cyb + cy); dot(&p, cxr + cy, cyb + cx);
    dot(&p, cxl - cx, cyb + cy); dot(&p, cxl - cy, cyb + cx);
    dot(&p, cxr + cx, cyt - cy); dot(&p, cxr + cy, cyt - cx);
    dot(&p, cxl - cx, cyt - cy); dot(&p, cxl - cy, cyt - cx);
    cy++;
    if (err < 0) {
      err += 2 * cy + 1;
    } else {
      cx--;
      err += 2 * (cy - cx) + 1;
    }
  }
}

/* ---------------------------------------------------------------- */
/* Triangle: sort by y, then walk the two edge pairs as spans.       */
/* ---------------------------------------------------------------- */

static void swap_pt(int16_t *ax, int16_t *ay, int16_t *bx, int16_t *by) {
  int16_t t;
  t = *ax; *ax = *bx; *bx = t;
  t = *ay; *ay = *by; *by = t;
}

static int edge_x(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int y) {
  if (y1 == y0) return x0;
  return x0 + (int)(x1 - x0) * (y - y0) / (y1 - y0);
}

void Gfx::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                       uint16_t color) {
  if (y0 > y1) swap_pt(&x0, &y0, &x1, &y1);
  if (y1 > y2) swap_pt(&x1, &y1, &x2, &y2);
  if (y0 > y1) swap_pt(&x0, &y0, &x1, &y1);

  lcd_pen_t p = lcd_pen(color);
  if (y0 == y2) {  /* degenerate: a single horizontal run */
    int lo = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int hi = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    span(&p, y0, lo, hi);
    return;
  }

  for (int y = y0; y <= y2; y++) {
    int a = edge_x(x0, y0, x2, y2, y);  /* the long edge */
    int b = (y < y1) ? edge_x(x0, y0, x1, y1, y) : edge_x(x1, y1, x2, y2, y);
    span(&p, y, a < b ? a : b, a < b ? b : a);
  }
}

/* ---------------------------------------------------------------- */
/* Text: font8x8_basic scaled by setTextSize(), background left as-is */
/* ---------------------------------------------------------------- */

void Gfx::setCursor(int16_t x, int16_t y) { cursor_x_ = x; cursor_y_ = y; }
void Gfx::setTextColor(uint16_t color) { text_color_ = color; }
void Gfx::setTextSize(uint8_t size) { text_size_ = size ? size : 1; }

static void draw_glyph(const lcd_pen_t *p, char c, int x, int y, int scale) {
  const char *glyph = font8x8_basic[(uint8_t)c & 0x7F];
  for (int row = 0; row < GFX_GLYPH_H; row++) {
    for (int col = 0; col < GFX_GLYPH_W; col++) {
      if (!(glyph[row] & (1 << col))) continue;
      if (scale == 1) {
        dot(p, x + col, y + row);
        continue;
      }
      for (int sy = 0; sy < scale; sy++)
        span(p, y + row * scale + sy, x + col * scale, x + col * scale + scale - 1);
    }
  }
}

/* font8x8_basic is ASCII. Every byte of a Hangul syllable in UTF-8 is >= 0x80,
 * so masking it into that table draws confetti -- which is what the Korean
 * strings would have done, silently, since nothing here would have failed. */
static bool needs_unicode(const char *s) {
  for (; *s; s++)
    if ((uint8_t)*s & 0x80) return true;
  return false;
}

void Gfx::print(const char *s) {
  if (!s) return;

  /* Non-Latin text goes through the launcher's own renderer, which is resident
   * firmware and already loads the Hangul font from the card. It has no
   * setTextSize equivalent, so Korean draws at one fixed size while Latin still
   * scales -- worth seeing in the harness before trusting a layout. */
  if (needs_unicode(s)) {
    cursor_x_ += tamapoke_draw_unicode(cursor_x_, cursor_y_, s, text_color_);
    return;
  }

  lcd_pen_t p = lcd_pen(text_color_);
  int adv = GFX_GLYPH_W * text_size_;
  for (; *s; s++) {
    if (*s == '\n') {
      cursor_x_ = 0;
      cursor_y_ += GFX_GLYPH_H * text_size_;
      continue;
    }
    draw_glyph(&p, *s, cursor_x_, cursor_y_, text_size_);
    cursor_x_ += adv;
  }
}

void Gfx::print(int v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", v);
  print(buf);
}

void Gfx::printf(const char *fmt, ...) {
  char buf[TEXT_BUF_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  print(buf);
}

void Gfx::writePixels(int16_t x, int16_t y, const uint16_t *px, int16_t count) {
  if (!px || y < 0 || y >= GFX_HEIGHT) return;
  for (int i = 0; i < count; i++) {
    lcd_pen_t p = lcd_pen(px[i]);
    dot(&p, x + i, y);
  }
}
