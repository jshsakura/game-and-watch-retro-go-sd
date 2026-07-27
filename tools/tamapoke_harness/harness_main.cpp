/* Render every TamaPoke screen on the host and gate the layout.
 *
 * The relayout from a 466x466 circle to 320x240 is the bulk of this port, and
 * flashing a unit to look at a misplaced button is the slowest possible way to
 * do it. This walks the screens, writes each one out as a PPM to eyeball, and
 * fails on the two things that are easy to ship without noticing:
 *
 *   - something drew outside the panel (guard bands either side of the
 *     framebuffer, so a primitive that skipped clipping is caught, not hidden)
 *   - the focus highlight is invisible on a screen, which on a device with no
 *     touch means that screen simply cannot be used
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dex.h"
#include "i18n.h"
#include "tamapoke_audio.h"
#include "tamapoke_input.h"
#include "tamapoke_sprites.h"
#include "tamapoke_ui.h"

/* Same unwrapped-include dance tamapoke_input.cpp documents: odroid_input.h has
 * no extern "C" guard of its own and the firmware compiles it as C. */
extern "C" {
#include "odroid_input.h"
}

extern void harness_fb_reset(uint16_t fill);
extern int harness_fb_guard_violations(void);
extern const uint16_t *harness_fb(void);
extern void harness_pad_clear(void);
extern void harness_pad_set(int key, bool down);

#define FB_W 320
#define FB_H 240
#define FB_PIXELS (FB_W * FB_H)

static uint16_t g_snapshot[FB_PIXELS];

static void write_ppm(const char *dir, const char *name, const uint16_t *fb) {
  char path[256];
  snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "  cannot write %s\n", path);
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
  for (int i = 0; i < FB_PIXELS; i++) {
    uint16_t c = fb[i];
    uint8_t r = (uint8_t)((c >> 11) & 0x1F), g = (uint8_t)((c >> 5) & 0x3F), b = (uint8_t)(c & 0x1F);
    uint8_t px[3] = {(uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)),
                     (uint8_t)((b << 3) | (b >> 2))};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
}

/* Render one screen at a chosen focus index into the live framebuffer. */
static void render_at(int screen, uint8_t focus) {
  harness_fb_reset(0x0000);
  tamapoke_ui_goto_screen(screen);
  tamapoke_input_reset(focus);
  tamapoke_ui_render();
}

/* A screen whose pixels do not change when focus moves has no visible cursor.
 * Comparing two focus positions is a cheap, generic way to prove the highlight
 * exists without knowing how any particular screen chose to draw it. */
static bool focus_is_visible(int screen) {
  render_at(screen, 0);
  memcpy(g_snapshot, harness_fb(), sizeof(g_snapshot));

  const focus_set_t *fs = tamapoke_current_focus_set();
  if (!fs || fs->count < 2) return true; /* nothing to move between */

  render_at(screen, 1);
  return memcmp(g_snapshot, harness_fb(), sizeof(g_snapshot)) != 0;
}

int main(int argc, char **argv) {
  const char *out_dir = argc > 1 ? argv[1] : "build/tamapoke_screens";

  sdReady = true; /* the packs are read through plain stdio on the host */
  harness_pad_clear();
  audioBegin();
  /* Same order the firmware uses. Skipping it is how the release dialog came
   * out reading "Release (null)?" -- DEX_TBL ships with null names and this is
   * what points them at the fallback. */
  tamapoke_dex_load_names();
  tamapoke_ui_init();

  /* Every screen is rendered in each language. A layout that fits in English
   * says nothing about Korean: the strings are longer, and Korean draws at a
   * fixed size while Latin scales, so the two are not the same picture. */
  static const struct { Lang lang; const char *tag; } LANGS[] = {
      {LANG_EN, "en"}, {LANG_KO, "ko"},
  };

  int failures = 0;
  for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
  gLang = LANGS[li].lang;
  for (int s = 0; s < TAMAPOKE_SCREEN_COUNT; s++) {
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s_%s", LANGS[li].tag,
             tamapoke_ui_screen_name(s));
    const char *name = name_buf;

    render_at(s, 0);
    int guard = harness_fb_guard_violations();
    write_ppm(out_dir, name, harness_fb());

    /* Also dump a focused frame. Proving that focus changes pixels is not the
     * same as showing what the cursor looks like, and a highlight can pass the
     * diff while being invisible to a person -- which is exactly how the action
     * buttons read as unselectable for a whole round of review. */
    const focus_set_t *fs0 = tamapoke_current_focus_set();
    if (fs0 && fs0->count > 1) {
      char focus_name[80];
      snprintf(focus_name, sizeof(focus_name), "%s_focus", name);
      render_at(s, 1);
      guard += harness_fb_guard_violations();
      write_ppm(out_dir, focus_name, harness_fb());
    }

    bool focus_ok = focus_is_visible(s);

    if (guard) {
      printf("FAIL %-14s drew outside the panel (%d guard words clobbered)\n", name, guard);
      failures++;
    } else if (!focus_ok) {
      printf("FAIL %-14s focus highlight is invisible -- unusable without touch\n", name);
      failures++;
    } else {
      printf("ok   %-18s\n", name);
    }
  }
  }

  /* Press every button on every screen.
   *
   * This file compiled tamapoke_input.cpp from day one and never called into it,
   * so the whole button front-end -- the only way anyone controls this port --
   * was unexercised. What shipped as a result: tamapoke_current_focus_set()
   * answered nullptr for the starter, card, clock, game and sack screens, and
   * tamapoke_input_poll() dereferenced it. On the device that read address 0,
   * found ITCM code there, and the halfword load off that odd value raised a
   * UsageFault (CFSR=0x01000000, UNALIGNED): the game died on any keypress, and
   * the starter screen -- the first thing a new save shows -- could not be used
   * at all because nothing was focusable.
   *
   * Every one of those is a plain NULL dereference that this build's ASan and
   * -fsanitize=alignment catch on the first call. The bug was never subtle; it
   * was simply never run. Rendering a screen is not using it.
   *
   * Buttons are pressed as edges (down then up) because the port acts on edges,
   * and the poll is called twice per press so the release is seen too. */
  static const struct { int key; const char *name; } KEYS[] = {
      {ODROID_INPUT_LEFT, "LEFT"},   {ODROID_INPUT_RIGHT, "RIGHT"},
      {ODROID_INPUT_UP, "UP"},       {ODROID_INPUT_DOWN, "DOWN"},
      {ODROID_INPUT_A, "A"},         {ODROID_INPUT_B, "B"},
  };
  int input_failures = 0;
  gLang = LANG_EN;
  for (int s = 0; s < TAMAPOKE_SCREEN_COUNT; s++) {
    for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
      tamapoke_ui_goto_screen(s);
      harness_pad_clear();
      tamapoke_input_reset(0);

      /* A focus set is mandatory, not optional: the input layer walks whatever
       * it is handed, so nullptr is a crash rather than "no focus here". */
      const focus_set_t *fs = tamapoke_current_focus_set();
      if (fs == NULL) {
        printf("FAIL %-14s has no focus set -- tamapoke_input_poll() would "
               "dereference NULL on any keypress\n",
               tamapoke_ui_screen_name(s));
        input_failures++;
        break;
      }

      /* An index past the end must not be read either: a screen transition can
       * leave a focus index the new screen does not have. */
      tamapoke_input_reset((uint8_t)(fs->count + 3));
      harness_pad_set(KEYS[k].key, true);
      tamapoke_input_poll(1000);
      harness_pad_set(KEYS[k].key, false);
      tamapoke_input_poll(1016);

      tamapoke_input_reset(0);
      harness_pad_set(KEYS[k].key, true);
      tamapoke_input_poll(2000);
      harness_pad_set(KEYS[k].key, false);
      tamapoke_input_poll(2016);

      if (harness_fb_guard_violations()) {
        printf("FAIL %-14s %s drew outside the panel\n",
               tamapoke_ui_screen_name(s), KEYS[k].name);
        input_failures++;
      }
    }
  }
  if (!input_failures)
    printf("\nok   every screen survives every button, incl. a stale focus index\n");
  failures += input_failures;

  int total = TAMAPOKE_SCREEN_COUNT * (int)(sizeof(LANGS) / sizeof(LANGS[0]));
  printf("\n%d/%d screens clean, PPMs in %s\n", total - failures, total, out_dir);
  return failures ? 1 : 0;
}
