/* Button front-end for the touch UI. See tamapoke_input.cpp for the rationale;
 * the short version is that the game's hit-testing is kept and fed synthesised
 * coordinates, so onTap() and friends never learn the panel is gone.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARRAY_LEN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

/* A rectangle that used to be touchable, in the UI's own coordinate space. */
typedef struct {
  int16_t x, y, w, h;
} hitbox_t;

/* One screen's worth of focusable rectangles.
 *
 * `boxes` may be NULL for regular lattices (the gallery, the keyboard), which
 * are derived from their cell geometry instead of tabulated -- a table of 30
 * keys drifts from the draw code the first time a cell size changes.
 *
 * `cols` is 0 for a list (left/right advances it) and non-zero for a grid
 * (the D-pad walks both axes; running off the side is a page turn).
 *
 * `kind` also decides what happens at the ends. Only the main screen turns an
 * overrun into upstream's swipe; a modal list has to CLAMP, or walking one step
 * past the last item throws the player out of the menu they were using. Every set
 * was kind 0 at first, so the feed menu jumped to the card screen on one extra
 * press of RIGHT -- and the egg, with a single item, did it on any press at all.
 *
 * `kind` tags the lattice family so grid_cell() can pick the right formula
 * without comparing set pointers across translation units: 0 = a plain list
 * (use `boxes`), 1 = the gallery lattice, 2 = the keyboard lattice. */
/* Values for focus_set_t::kind. */
#define FOCUS_KIND_LIST     0  /* the main screen: an overrun is a swipe */
#define FOCUS_KIND_GALLERY  1
#define FOCUS_KIND_KEYBOARD 2
#define FOCUS_KIND_MODAL    3  /* a dialog or menu: clamp at both ends */

typedef struct {
  const hitbox_t *boxes;
  uint8_t count;
  uint8_t cols;
  uint8_t kind;   /* 0=list, 1=gallery, 2=keyboard */
} focus_set_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per frame with the port's millisecond clock. */
void tamapoke_input_poll(uint32_t now_ms);

/* Index of the focused widget, for the draw code's highlight. */
uint8_t tamapoke_input_focus(void);

/* Point focus at a known-visible widget. Every screen transition must call
 * this: a focus index left over from the previous screen is either invisible
 * or, worse, pointing at a different action than the one it highlights. */
void tamapoke_input_reset(uint8_t focus);

/* Supplied by the UI port (tamapoke_ui.cpp). */
const focus_set_t *tamapoke_current_focus_set(void);
bool tamapoke_is_dimmed(void);
void tamapoke_wake(void);
void tamapoke_hold_release(void);

#ifdef __cplusplus
}
#endif
