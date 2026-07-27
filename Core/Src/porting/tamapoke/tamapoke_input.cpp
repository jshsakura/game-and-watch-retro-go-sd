/* Button front-end for TamaPoke's touch UI.
 *
 * The upstream game is driven entirely by a capacitive panel: every screen
 * resolves input by hit-testing a coordinate (onTap) or by classifying a
 * swipe (onSwipe/onSwipeV). None of that logic is replaced here. The panel
 * driver is gone; the hit-testing is not.
 *
 * What this file does is stand in for the finger. Each screen declares the
 * rectangles that were already touchable, the D-pad walks between them, and
 * A synthesises a tap at the focused rectangle's centre and hands it to the
 * game's own onTap(). From onTap() down, upstream code runs unmodified --
 * which is also what lets us take upstream changes later.
 */
#include "tamapoke_input.h"

#include <string.h>

/* odroid_input.h has no extern "C" guard of its own and the firmware compiles
 * it as C, so including it unwrapped from C++ mangles odroid_input_read_gamepad
 * and the link fails. The harness stub must wrap it the same way. */
extern "C" {
#include "odroid_input.h"
}

#include "tamapoke_ui.h"  /* onTap / onSwipe / onSwipeV / onBack, from the port */

/* A press must be an edge, not a level, or one frame of contact fires the
 * action for as long as the button is down. */
#define HOLD_RELEASE_MS 3000  /* upstream's 3 s hold on the pet = release dialog */

typedef struct {
  odroid_gamepad_state_t prev;
  uint32_t b_down_at;   /* 0 when B is up */
  bool b_hold_fired;    /* upstream's holdFired: one dialog per hold */
  uint8_t focus;
} input_state_t;

static input_state_t g_in;

/* ------------------------------------------------------------------ */
/* Focus sets: declared here, defined by the UI port.                  */
/*                                                                    */
/* The focus sets themselves (FOCUS_MAIN, FOCUS_GALLERY, FOCUS_KEYBOARD)*/
/* are constructed in tamapoke_ui.cpp, which owns the screen state and  */
/* knows which one is active. grid_cell() below identifies a lattice   */
/* by the `kind` tag rather than by pointer, so the two translation     */
/* units stay independent.                                             */
/* ------------------------------------------------------------------ */

/* Grid cells are derived rather than tabulated -- 30 keys and 16 gallery
 * cells are a regular lattice, and a table of them would drift from the
 * draw code the first time a cell size changes. */
static hitbox_t grid_cell(const focus_set_t *fs, uint8_t i) {
  hitbox_t b = {0, 0, 0, 0};
  if (fs->kind == 1) {  /* gallery */
    b.x = GAL_X + (i % GAL_COLS) * GAL_CELL;
    b.y = GAL_Y + (i / GAL_COLS) * GAL_CELL;
    b.w = b.h = GAL_CELL;
  } else if (fs->kind == 2) {  /* keyboard */
    b.x = KB_X + (i % KB_COLS) * KB_W;
    b.y = KB_Y + (i / KB_COLS) * KB_H;
    b.w = KB_W;
    b.h = KB_H;
  }
  return b;
}

static hitbox_t focus_box(const focus_set_t *fs, uint8_t i) {
  return fs->boxes ? fs->boxes[i] : grid_cell(fs, i);
}

/* ------------------------------------------------------------------ */

static bool edge(const odroid_gamepad_state_t *now, int key) {
  return now->values[key] && !g_in.prev.values[key];
}

/* ODROID_INPUT_ANY sits *past* ODROID_INPUT_MAX, so it is a sentinel and not
 * an index -- values[ODROID_INPUT_ANY] reads one element off the end of the
 * array. Ask the question by scanning instead. */
static bool any_key_down(const odroid_gamepad_state_t *js) {
  for (int i = 0; i < ODROID_INPUT_MAX; i++)
    if (js->values[i]) return true;
  return false;
}

/* Linear sets walk with left/right; grids walk in both axes and clamp,
 * so the caller can turn an overrun into a page turn. */
static int focus_step(const focus_set_t *fs, int dx, int dy) {
  int n = fs->count;
  if (n <= 0) return 0;

  if (fs->cols == 0) {
    int next = (int)g_in.focus + dx + dy;  /* a list: any axis advances it */
    if (next < 0 || next >= n) return next < 0 ? -1 : +1;
    g_in.focus = (uint8_t)next;
    return 0;
  }

  int col = g_in.focus % fs->cols, row = g_in.focus / fs->cols;
  int rows = (n + fs->cols - 1) / fs->cols;
  col += dx;
  row += dy;
  if (col < 0 || col >= fs->cols) return col < 0 ? -1 : +1;  /* page turn */
  if (row < 0) row = 0;
  if (row >= rows) row = rows - 1;

  int next = row * fs->cols + col;
  if (next < n) g_in.focus = (uint8_t)next;
  return 0;
}

/* The whole point: build a coordinate and let upstream resolve it. */
static void focus_activate(const focus_set_t *fs) {
  hitbox_t b = focus_box(fs, g_in.focus);
  onTap(b.x + b.w / 2, b.y + b.h / 2);
}

void tamapoke_input_reset(uint8_t focus) {
  g_in.focus = focus;
  g_in.b_down_at = 0;
  g_in.b_hold_fired = false;
}

uint8_t tamapoke_input_focus(void) { return g_in.focus; }

void tamapoke_input_poll(uint32_t now_ms) {
  odroid_gamepad_state_t js;
  odroid_input_read_gamepad(&js);

  const focus_set_t *fs = tamapoke_current_focus_set();

  /* Upstream swallows the first touch after the screen dims and only wakes
   * up. Keep that: any key while dimmed is a wake, not an action. */
  if (tamapoke_is_dimmed()) {
    if (any_key_down(&js)) tamapoke_wake();
    g_in.prev = js;
    return;
  }

  int overflow = 0;
  if (edge(&js, ODROID_INPUT_LEFT)) overflow = focus_step(fs, -1, 0);
  if (edge(&js, ODROID_INPUT_RIGHT)) overflow = focus_step(fs, +1, 0);
  if (edge(&js, ODROID_INPUT_UP)) overflow = focus_step(fs, 0, -1);
  if (edge(&js, ODROID_INPUT_DOWN)) overflow = focus_step(fs, 0, +1);

  /* Walking off the side of a paged screen is upstream's horizontal swipe;
   * on the main screen up/down are the vertical swipes (card / clock). */
  if (overflow) {
    if (fs->cols) {
      onSwipe(overflow > 0 ? -1 : +1);
      g_in.focus = 0;
    } else if (fs->kind == 0) {  /* main screen list: up/down = vertical swipe */
      onSwipeV(overflow > 0 ? +1 : -1);
    }
  }

  if (edge(&js, ODROID_INPUT_A)) focus_activate(fs);

  /* B is back/cancel. Touch had no "outside the dialog" to tap, so this is
   * the one gesture the port must add rather than translate. */
  if (edge(&js, ODROID_INPUT_B)) onBack();

  /* ...and B held is upstream's 3 s press on the pet: the release dialog. */
  if (js.values[ODROID_INPUT_B]) {
    if (!g_in.b_down_at) g_in.b_down_at = now_ms;
    if (!g_in.b_hold_fired && now_ms - g_in.b_down_at > HOLD_RELEASE_MS) {
      tamapoke_hold_release();
      g_in.b_hold_fired = true;
    }
  } else {
    g_in.b_down_at = 0;
    g_in.b_hold_fired = false;
  }

  g_in.prev = js;
}
