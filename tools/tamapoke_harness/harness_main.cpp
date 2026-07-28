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
#include <stdint.h>
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
/* uint64_t, not unsigned long long: on LP64 they are distinct types and C++
 * mangles the declaration into a symbol the stubs never defined. */
extern void harness_clock_set_ms(uint64_t ms);
extern void harness_clock_advance_ms(uint64_t ms);
extern uint64_t harness_clock_ms(void);

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

  /* And in each theme. No capture ever showed the NIGHT theme, which is how a
   * white slab with three invisible lines of stats on it reached hardware: the
   * surfaces are fixed colours, the ink followed the theme, and only one of the two
   * was ever looked at. The clock picks the theme (sceneHour), so this is 13:00 and
   * 01:00. */
  static const struct { const char *tag; unsigned long long at_ms; } THEMES[] = {
      {"", 13ull * 3600 * 1000}, {"_night", 1ull * 3600 * 1000},
  };

  int failures = 0;
  for (size_t li = 0; li < sizeof(LANGS) / sizeof(LANGS[0]); li++) {
  gLang = LANGS[li].lang;
  for (size_t ti = 0; ti < sizeof(THEMES) / sizeof(THEMES[0]); ti++) {
  harness_clock_set_ms(THEMES[ti].at_ms);
  for (int s = 0; s < TAMAPOKE_SCREEN_COUNT; s++) {
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s_%s%s", LANGS[li].tag,
             tamapoke_ui_screen_name(s), THEMES[ti].tag);
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

  /* No screen may inherit the previous screen's pixels.
   *
   * The device keeps the canvas between frames (lcd_clone) so the UI can draw
   * incrementally, which means a region a screen never paints still shows
   * whatever drew there last. render_at() above resets the framebuffer before
   * every render, so this harness could not see that: it was measuring each
   * screen in a cleanroom the device never provides.
   *
   * What it hid: the main screen's egg branch returned before filling y=152..240,
   * so the third starter row -- outline, sprite and the word SQUIRTLE -- sat
   * underneath the egg on hardware, and coming back from the pause menu left that
   * band black.
   *
   * The test is the same picture twice: render B on a clean canvas, then render B
   * again on top of A. Anything that differs is a pixel B does not own. */
  /* A timed overlay must survive being navigated to.
   *
   * Upstream's feed menu closes 3 s after it opens, which is generous for a tap
   * on the item and not enough for a cursor that has to walk there first: on
   * hardware the menu shut under the player and the food could not be chosen at
   * all. Nothing here noticed, because every earlier check either rendered a
   * frozen frame or pressed a button and only asked that it did not crash.
   *
   * So: open it, spend time walking as a person would, then press A and require
   * that something was actually eaten. The clock is advanced explicitly, which is
   * the only way a host test can be honest about a timeout. */
  int timeout_failures = 0;
  {
    harness_clock_set_ms(100000);
    tamapoke_ui_goto_screen(0);          /* main screen, pet already hatched */
    harness_pad_clear();
    tamapoke_input_reset(0);

    /* FOCUS_MAIN is {pet zone, BTN0..BTN3} and BTN0 is the meal, so one step
     * right from the default lands on it. */
    harness_pad_set(ODROID_INPUT_RIGHT, true);
    tamapoke_input_poll((uint32_t)harness_clock_ms());
    harness_pad_set(ODROID_INPUT_RIGHT, false);
    tamapoke_input_poll((uint32_t)harness_clock_ms());
    harness_pad_set(ODROID_INPUT_A, true);
    tamapoke_input_poll((uint32_t)harness_clock_ms());
    harness_pad_set(ODROID_INPUT_A, false);
    tamapoke_input_poll((uint32_t)harness_clock_ms());

    bool opened = (tamapoke_current_focus_set() == tamapoke_focus_set_feed());
    if (!opened) {
      printf("FAIL the feed menu did not open from the button row\n");
      timeout_failures++;
    } else {
      /* Walk for LONGER than the overlay's own duration (FEED_MENU_MS), one
       * press per second. Anything shorter would pass on the duration alone and
       * would still pass with the renew deleted -- which is exactly what the
       * first version of this check did, so it pinned nothing. */
      for (int i = 0; i < 9; i++) {
        harness_clock_advance_ms(1000);
        harness_pad_set(ODROID_INPUT_RIGHT, true);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
        harness_pad_set(ODROID_INPUT_RIGHT, false);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
      }
      if (tamapoke_current_focus_set() != tamapoke_focus_set_feed()) {
        printf("FAIL the feed menu expired while the cursor was still walking to "
               "an item -- this is the 'cannot select food' report\n");
        timeout_failures++;
      } else {
        printf("ok   a timed overlay survives being navigated\n");
      }
    }
    /* And overshooting the last item must not eject the player. focus_step()
     * reports the overrun; on the main screen that is upstream's vertical swipe,
     * and every set used to be that kind -- so one press past the last food
     * opened the card screen in the middle of choosing. */
    if (!timeout_failures) {
      for (int i = 0; i < 8; i++) {   /* well past the five items */
        harness_pad_set(ODROID_INPUT_RIGHT, true);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
        harness_pad_set(ODROID_INPUT_RIGHT, false);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
      }
      if (tamapoke_current_focus_set() != tamapoke_focus_set_feed()) {
        printf("FAIL walking past the last feed item left the menu -- an overrun "
               "in a modal list must clamp, not swipe\n");
        timeout_failures++;
      } else {
        printf("ok   a modal menu clamps at its ends instead of ejecting\n");
      }
    }
  }
  failures += timeout_failures;

  /* Every sub-screen must be reachable with the buttons.
   *
   * This is the check whose absence hid the biggest gap in the port: the only code
   * that set cardOpen / galleryOpen / clockOpen / gameOpen was
   * tamapoke_ui_goto_screen(), which exists for THIS HARNESS. So all six rendered
   * perfectly here and not one of them could be opened on the device -- including
   * the settings screen, which is why its language pill could not be used. The
   * renderers were fine. Nothing led to them.
   *
   * Driving the buttons from the main screen is the only way to see that.
   */
  int nav_failures = 0;
  {
    struct Nav { const char *what; int key; int presses; int want_screen; };
    /* One key per screen, from the main screen.
     *
     * TIME and GAME are printed on the console next to the display and were both
     * dead; they now open the two screens they name. UP is the status card and DOWN
     * is the Pokedex. What this replaces: DOWN opened the settings screen and the
     * Pokedex was "press RIGHT six times until you fall off the end of the button
     * row" -- which also fired every time a player overshot the last button, so
     * overshooting ejected you onto another screen. LEFT/RIGHT now clamp; the check
     * below pins that. */
    const Nav navs[] = {
      {"clock/settings (TIME)", ODROID_INPUT_SELECT, 1, 4},
      {"the minigame (GAME)",   ODROID_INPUT_START,  1, 6},
      {"status card (UP)",      ODROID_INPUT_UP,     1, 1},
      {"the Pokedex (DOWN)",    ODROID_INPUT_DOWN,   1, 2},
    };
    for (size_t i = 0; i < sizeof(navs) / sizeof(navs[0]); i++) {
      harness_clock_set_ms(200000 + i * 10000);
      tamapoke_ui_goto_screen(0);          /* start on main, pet hatched */
      tamapoke_ui_release_harness_screen();/* stop forcing, so state decides */
      harness_pad_clear();
      tamapoke_input_reset(0);

      for (int p2 = 0; p2 < navs[i].presses; p2++) {
        harness_pad_set(navs[i].key, true);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
        harness_pad_set(navs[i].key, false);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
      }

      int got = tamapoke_ui_current_screen();
      if (got != navs[i].want_screen) {
        printf("FAIL %s is not reachable with the buttons (screen %d, wanted %d)\n",
               navs[i].what, got, navs[i].want_screen);
        nav_failures++;
      } else {
        printf("ok   %s opens from the main screen\n", navs[i].what);
      }
    }
  }
  failures += nav_failures;

  /* Walking off the end of the button row must stay on the button row.
   *
   * It used to be upstream's horizontal swipe, which opened the Pokedex. A swipe is
   * a deliberate gesture on a panel; with a cursor it is what happens whenever
   * someone presses RIGHT once too often, so the game changed screens as a
   * punishment for overshooting. */
  {
    harness_clock_set_ms(300000);
    tamapoke_ui_goto_screen(0);
    tamapoke_ui_release_harness_screen();
    harness_pad_clear();
    tamapoke_input_reset(0);
    for (int i = 0; i < 10; i++) {   /* five widgets, ten presses */
      harness_pad_set(ODROID_INPUT_RIGHT, true);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      harness_pad_set(ODROID_INPUT_RIGHT, false);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
    }
    if (tamapoke_ui_current_screen() != 0) {
      printf("FAIL overshooting the last action button left the main screen "
             "(screen %d) -- an overrun must clamp\n", tamapoke_ui_current_screen());
      failures++;
    } else {
      printf("ok   overshooting the button row stays on the main screen\n");
    }
  }

  /* The ball minigame must be playable.
   *
   * paddleX was written exactly once, in startGame(), and never again -- no key,
   * no touch, nothing moved it. The game shipped three times as a ball bouncing off
   * a decoration. */
  int game_failures = 0;
  {
    harness_clock_set_ms(400000);
    tamapoke_ui_goto_screen(0);
    tamapoke_ui_release_harness_screen();
    harness_pad_clear();
    tamapoke_input_reset(0);
    harness_pad_set(ODROID_INPUT_START, true);
    tamapoke_input_poll((uint32_t)harness_clock_ms());
    harness_pad_set(ODROID_INPUT_START, false);
    tamapoke_input_poll((uint32_t)harness_clock_ms());

    tamapoke_probe_t p0, p1;
    tamapoke_ui_probe(&p0);
    if (p0.screen != 6) {
      printf("FAIL GAME did not open the minigame (screen %d)\n", p0.screen);
      game_failures++;
    } else {
      /* Hold RIGHT for a while. The paddle is stepped by the tick, not the poll,
       * so both have to run -- which is also the contract that keeps its speed
       * independent of how fast the main loop happens to spin. */
      harness_pad_set(ODROID_INPUT_RIGHT, true);
      for (int i = 0; i < 6; i++) {
        harness_clock_advance_ms(34);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
        tamapoke_ui_tick((uint32_t)harness_clock_ms());
      }
      harness_pad_set(ODROID_INPUT_RIGHT, false);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      tamapoke_ui_probe(&p1);

      if (p1.paddle_x <= p0.paddle_x) {
        printf("FAIL holding RIGHT did not move the paddle (%d -> %d) -- the "
               "minigame cannot be played\n", p0.paddle_x, p1.paddle_x);
        game_failures++;
      } else if (p1.paddle_x > GAME_PADDLE_X_MAX) {
        printf("FAIL the paddle left the playfield (%d > %d)\n",
               p1.paddle_x, GAME_PADDLE_X_MAX);
        game_failures++;
      } else {
        /* And the other way, back past where it started. */
        harness_pad_set(ODROID_INPUT_LEFT, true);
        for (int i = 0; i < 12; i++) {
          harness_clock_advance_ms(34);
          tamapoke_input_poll((uint32_t)harness_clock_ms());
          tamapoke_ui_tick((uint32_t)harness_clock_ms());
        }
        harness_pad_set(ODROID_INPUT_LEFT, false);
        tamapoke_probe_t p2;
        tamapoke_ui_probe(&p2);
        if (p2.paddle_x >= p1.paddle_x || p2.paddle_x < GAME_PADDLE_X_MIN) {
          printf("FAIL holding LEFT did not move the paddle back (%d -> %d)\n",
                 p1.paddle_x, p2.paddle_x);
          game_failures++;
        } else {
          printf("ok   the minigame's paddle answers LEFT/RIGHT and stays in bounds\n");
        }
      }
    }
  }
  failures += game_failures;

  /* A round must end, and ending it must train the pet.
   *
   * pet.playResult() and pet.trainStrength() -- the two functions that turn a round
   * into training -- were dead code in the tree: nothing called either. So a round
   * had no ending (only B), no reward, and no record. */
  int reward_failures = 0;
  if (!game_failures) {
    tamapoke_probe_t before, after;
    tamapoke_ui_probe(&before);

    /* Phase 1: track the ball and rally it. Five returns is enough to be worth a
     * training point (playResult gives score/5), and it is also the only way to
     * prove the paddle can actually be used to play rather than merely to move. */
    for (int i = 0; i < 2000 && tamapoke_ui_current_screen() == 6; i++) {
      tamapoke_probe_t p;
      tamapoke_ui_probe(&p);
      if (p.game_score >= 5) break;
      bool go_right = p.ball_x > p.paddle_x + GAME_PADDLE_W / 2;
      harness_pad_set(ODROID_INPUT_RIGHT, go_right);
      harness_pad_set(ODROID_INPUT_LEFT, !go_right);
      harness_clock_advance_ms(34);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      tamapoke_ui_tick((uint32_t)harness_clock_ms());
    }
    tamapoke_probe_t rallied;
    tamapoke_ui_probe(&rallied);
    if (rallied.game_score < 5) {
      printf("FAIL tracking the ball with the paddle scored %u in 2000 frames -- "
             "the ball cannot be returned\n", rallied.game_score);
      reward_failures++;
    }

    /* Phase 2: run away from it, so every descent is a miss and the third ends the
     * round. Parking the paddle and hoping would leave the test at the mercy of
     * where a bouncing ball happens to land, and the ball's horizontal speed changes
     * on every hit -- "it will miss eventually" is not something this can assume. */
    for (int i = 0; i < 2000 && tamapoke_ui_current_screen() == 6; i++) {
      tamapoke_probe_t p;
      tamapoke_ui_probe(&p);
      bool ball_right = p.ball_x > FB_W / 2;
      harness_pad_set(ODROID_INPUT_LEFT, ball_right);
      harness_pad_set(ODROID_INPUT_RIGHT, !ball_right);
      harness_clock_advance_ms(34);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      tamapoke_ui_tick((uint32_t)harness_clock_ms());
    }
    harness_pad_clear();
    tamapoke_ui_probe(&after);
    if (tamapoke_ui_current_screen() == 6) {
      printf("FAIL the ball round never ended -- three misses must finish it\n");
      reward_failures++;
    } else if (after.pet_tr_spe <= before.pet_tr_spe) {
      printf("FAIL a %u-point round trained nothing (SPE %u -> %u) -- "
             "pet.playResult() is still unreachable\n",
             rallied.game_score, before.pet_tr_spe, after.pet_tr_spe);
      reward_failures++;
    } else if (!reward_failures) {
      printf("ok   a ball round can be rallied, ends on three misses, and trains "
             "the pet (SPE %u -> %u)\n", before.pet_tr_spe, after.pet_tr_spe);
    }
  }
  failures += reward_failures;

  /* The training sack must count hits and bank them.
   *
   * sackHits was never incremented by anything: A did nothing, the ten-second timer
   * ran out, and the screen sat there for ever because sackOverUntil was never set
   * either. */
  int sack_failures = 0;
  {
    harness_clock_set_ms(500000);
    tamapoke_ui_goto_screen(7);
    tamapoke_ui_release_harness_screen();
    harness_pad_clear();
    tamapoke_input_reset(0);

    tamapoke_probe_t p0;
    tamapoke_ui_probe(&p0);
    const uint16_t hits0 = p0.sack_hits;
    for (int i = 0; i < 12; i++) {
      harness_clock_advance_ms(40);
      harness_pad_set(ODROID_INPUT_A, true);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      harness_pad_set(ODROID_INPUT_A, false);
      tamapoke_input_poll((uint32_t)harness_clock_ms());
      tamapoke_ui_tick((uint32_t)harness_clock_ms());
    }
    tamapoke_probe_t p1;
    tamapoke_ui_probe(&p1);
    if (p1.sack_hits != hits0 + 12) {
      printf("FAIL pressing A did not hit the sack (%u -> %u, wanted %u)\n",
             hits0, p1.sack_hits, (unsigned)(hits0 + 12));
      sack_failures++;
    } else {
      /* Run the round out. The timer expiring is what banks the hits. */
      for (int i = 0; i < 500 && tamapoke_ui_current_screen() == 7; i++) {
        harness_clock_advance_ms(34);
        tamapoke_input_poll((uint32_t)harness_clock_ms());
        tamapoke_ui_tick((uint32_t)harness_clock_ms());
      }
      tamapoke_probe_t p2;
      tamapoke_ui_probe(&p2);
      if (tamapoke_ui_current_screen() == 7) {
        printf("FAIL the training round never ended -- the timer must close it\n");
        sack_failures++;
      } else if (p2.sack_gain == 0 || p2.pet_tr_atk <= p1.pet_tr_atk) {
        printf("FAIL the training round banked nothing (gain %u, ATK %u -> %u) -- "
               "pet.trainStrength() is still unreachable\n",
               p2.sack_gain, p1.pet_tr_atk, p2.pet_tr_atk);
        sack_failures++;
      } else {
        printf("ok   the sack counts A as a hit and the round trains strength\n");
      }
    }
  }
  failures += sack_failures;

  /* The bottom panel must be light enough for the ink drawn on it.
   *
   * The labels under it are drawn in inkColor(), which is DARK in the day theme,
   * and the panel was filled with an opaque #383844. Dark grey on dark navy: the
   * stat labels were reported unreadable from hardware, and no gate here could see
   * it because none of them looked at a colour. This measures the two and requires
   * them to be far apart -- which is the property that was violated, rather than
   * "the panel is white", which would just re-state today's choice. */
  {
    /* Both themes, because each has its own ink and the panel has to stay clear of
     * whichever one is in use. The clock is what selects the theme (sceneHour), so
     * these are 13:00 and 01:00 -- and a test that set "600000 ms" was quietly
     * measuring the night theme against the day theme's ink. */
    struct Theme { const char *name; uint64_t at_ms; int ink_luma; };
    const Theme themes[] = {
      {"day",   13ull * 3600 * 1000, 41},   /* UI_INK       #2a2a36 */
      {"night",  1ull * 3600 * 1000, 221},  /* UI_INK_NIGHT #d8dcf0 */
    };
    for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
      harness_clock_set_ms(themes[t].at_ms);
      tamapoke_ui_goto_screen(0);
      tamapoke_input_reset(0);
      tamapoke_ui_render();
      const uint16_t *fb = harness_fb();
      /* The label column, left of the first bar's track. */
      long sum = 0; int n = 0;
      for (int y = PANEL_Y + 4; y < GFX_HEIGHT - 40; y++)
        for (int x = 0; x < BAR_LABEL_GAP; x++) {
          uint16_t c = fb[y * FB_W + x];
          int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
          sum += (r * 30 + g * 59 + b * 11) / 100;   /* luma 0..255 */
          n++;
        }
      int panel_luma = n ? (int)(sum / n) : 0;
      int contrast = panel_luma > themes[t].ink_luma ? panel_luma - themes[t].ink_luma
                                                    : themes[t].ink_luma - panel_luma;
      if (contrast < 60) {
        printf("FAIL the %s stat panel (luma %d) is too close to the ink drawn on it "
               "(luma %d) -- the labels are unreadable\n",
               themes[t].name, panel_luma, themes[t].ink_luma);
        failures++;
      } else {
        printf("ok   the %s stat panel contrasts with its own labels (luma %d vs %d)\n",
               themes[t].name, panel_luma, themes[t].ink_luma);
      }
    }
  }

  /* No widget may be a flat rectangle -- in EITHER theme.
   *
   * A tile's surface is a fixed colour (a white pill, a beige well) and its label
   * used to be drawn in inkColor(), which follows the theme. Day theme: dark ink on
   * a light tile, fine. NIGHT theme: light ink on a light tile, and the label is
   * gone. On hardware the status card showed a blank white slab where three lines of
   * stats should be, and a blank white pill where BACK should be. Every screen in
   * the port had the same exposure and only the night theme showed it.
   *
   * Measured as flatness, which is what the eye sees: a widget that contains a label
   * has a luma spread inside its own rectangle. One that has swallowed its label is
   * uniform. That is mechanical -- no threshold on "is this pretty" -- and it fails
   * on exactly the thing that was broken.
   *
   * The focus sets give the pressable rectangles for free. The two wells are not
   * focusable, so they are named: they are the two biggest offenders. */
  {
    struct Rect { const char *what; int x, y, w, h; };
    struct Theme { const char *name; unsigned long long at_ms; };
    const Theme themes[] = {{"day", 13ull * 3600 * 1000}, {"night", 1ull * 3600 * 1000}};
    /* Wells, by name: the card's stat block and the keyboard's name preview. */
    const struct { int screen; Rect r; } WELLS[] = {
      {1, {"the card's stat block", CARD_STATS_X, CARD_STATS_Y, CARD_STATS_W, CARD_STATS_H}},
      {3, {"the keyboard's name box", KB_NAME_X, KB_NAME_Y, KB_NAME_W, KB_NAME_H}},
    };

    int flat_failures = 0;
    for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
      harness_clock_set_ms(themes[t].at_ms);

      /* Every focusable rectangle on every screen, at its own focus index so the
       * check sees the selected treatment too. */
      for (int sc = 0; sc < TAMAPOKE_SCREEN_COUNT; sc++) {
        tamapoke_ui_goto_screen(sc);
        const focus_set_t *fs = tamapoke_current_focus_set();
        if (!fs || fs->count == 0 || fs->boxes == NULL) continue;   /* lattices derive */
        for (uint8_t i = 0; i < fs->count; i++) {
          hitbox_t b = fs->boxes[i];
          if (b.w <= 4 || b.h <= 4) continue;
          /* BOTH states. Pointing the cursor at each widget in turn tests only its
           * SELECTED look, and the selected look is an accent surface that always
           * contrasts -- so the first version of this check passed the very tile
           * the photo showed as blank. The unfocused state is the one that broke. */
          for (int sel = 0; sel < 2; sel++) {
            uint8_t focus_at = sel ? i : (uint8_t)((i + 1) % fs->count);
            harness_fb_reset(0x0000);
            tamapoke_ui_goto_screen(sc);
            tamapoke_input_reset(focus_at);
            tamapoke_ui_render();
            int lo = 255, hi = 0;
            const uint16_t *fb = harness_fb();
            for (int y = b.y + 2; y < b.y + b.h - 2 && y < FB_H; y++)
              for (int x = b.x + 2; x < b.x + b.w - 2 && x < FB_W; x++) {
                uint16_t c = fb[y * FB_W + x];
                int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, bl = (c & 0x1F) << 3;
                int l = (r * 30 + g * 59 + bl * 11) / 100;
                if (l < lo) lo = l;
                if (l > hi) hi = l;
              }
            if (hi - lo < 40) {
              printf("FAIL %s widget %u on %s is flat %s (luma spread %d) -- its "
                     "label is invisible on its own surface\n",
                     themes[t].name, i, tamapoke_ui_screen_name(sc),
                     sel ? "when selected" : "when not selected", hi - lo);
              flat_failures++;
            }
          }
        }
      }

      for (size_t k = 0; k < sizeof(WELLS) / sizeof(WELLS[0]); k++) {
        harness_fb_reset(0x0000);
        tamapoke_ui_goto_screen(WELLS[k].screen);
        tamapoke_input_reset(0);
        tamapoke_ui_render();
        const Rect &r0 = WELLS[k].r;
        int lo = 255, hi = 0;
        const uint16_t *fb = harness_fb();
        for (int y = r0.y + 2; y < r0.y + r0.h - 2 && y < FB_H; y++)
          for (int x = r0.x + 2; x < r0.x + r0.w - 2 && x < FB_W; x++) {
            uint16_t c = fb[y * FB_W + x];
            int r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, bl = (c & 0x1F) << 3;
            int l = (r * 30 + g * 59 + bl * 11) / 100;
            if (l < lo) lo = l;
            if (l > hi) hi = l;
          }
        if (hi - lo < 40) {
          printf("FAIL %s: %s is flat (luma spread %d) -- what it contains cannot be "
                 "read on it\n", themes[t].name, r0.what, hi - lo);
          flat_failures++;
        }
      }
    }
    if (!flat_failures)
      printf("ok   no widget or well swallows its own label, in either theme\n");
    failures += flat_failures;
  }

  /* Pokedex thumbnails must be decoded the way the file is written.
   *
   * thumbs.bin entries are `u8 w, u8 h, u8 palCount, u16 pal[palCount],
   * u8 px[w*h]` with 0xFF transparent -- read back out of the shipped
   * tamapoke_assets.dat. drawThumb() instead read a fixed 24x24 block of raw bytes
   * and coloured each through spriteColor(), the ASCII-sprite palette: it drew the
   * header as pixels, ran ~240 bytes past the end of every record into the next
   * species, and looked the result up in the wrong table. Every one of the 151 was
   * corrupt, and no gate here noticed because the harness never loads the pack --
   * so the gallery it rendered was always the flash-sprite fallback.
   *
   * A synthetic pack, so this runs with no assets on the machine. */
  {
    static uint8_t pack[6 + 4 + 3 + 2 * 2 + 4 * 4];
    const uint16_t RED = 0xF800, GREEN = 0x07E0;
    memcpy(pack, "TPTH", 4);
    pack[4] = 1; pack[5] = 0;                      /* count = 1 */
    uint32_t off = 10;
    memcpy(pack + 6, &off, 4);                     /* entry 0 offset */
    pack[off + 0] = 4;                             /* w */
    pack[off + 1] = 4;                             /* h */
    pack[off + 2] = 2;                             /* palCount */
    pack[off + 3] = (uint8_t)(RED & 0xFF);
    pack[off + 4] = (uint8_t)(RED >> 8);
    pack[off + 5] = (uint8_t)(GREEN & 0xFF);
    pack[off + 6] = (uint8_t)(GREEN >> 8);
    for (int i = 0; i < 16; i++) pack[off + 7 + i] = 0xFF;  /* all transparent */
    pack[off + 7 + 0] = 0;                         /* top-left     = RED   */
    pack[off + 7 + 3] = 1;                         /* top-right    = GREEN */

    thumbs.data = pack;
    thumbs.size = sizeof(pack);
    thumbs.count = 1;
    thumbs.loaded = true;

    SdThumb t;
    bool ok = thumbs.get(1, &t);
    if (!ok || t.w != 4 || t.h != 4 || t.palCount != 2) {
      printf("FAIL a thumbnail entry does not parse (%d, %ux%u, pal %u)\n",
             (int)ok, t.w, t.h, t.palCount);
      failures++;
    } else {
      harness_fb_reset(0x0000);
      /* Centred on (100, 100) at scale 1: a 4x4 sprite puts its top-left at 98,98. */
      tamapoke_ui_draw_thumb(1, 100, 100, 1);
      const uint16_t *fb = harness_fb();
      uint16_t tl = fb[98 * FB_W + 98], tr = fb[98 * FB_W + 101];
      uint16_t mid = fb[99 * FB_W + 99];
      if (tl != RED || tr != GREEN || mid != 0x0000) {
        printf("FAIL a thumbnail draws the wrong pixels (tl %04x want %04x, "
               "tr %04x want %04x, transparent %04x want 0000)\n",
               tl, RED, tr, GREEN, mid);
        failures++;
      } else {
        printf("ok   Pokedex thumbnails decode w/h/palette and honour 0xFF\n");
      }
    }
    /* Leave the pack out of the way of the render checks below. */
    thumbs.loaded = false;
    thumbs.data = nullptr;
    thumbs.size = 0;
    thumbs.count = 0;
  }

  /* No screen may leave a region unpainted after a screen change.
   *
   * Done with a sentinel rather than by diffing two renders: rendering is not
   * idempotent (pets wander, cursors blink, the ball moves), so comparing render
   * #1 with render #2 flags animated screens and a gate that cannot tell its own
   * setup from a finding gets switched off. Fill the canvas with a colour no
   * screen draws, switch screens, render once -- every surviving sentinel pixel
   * is a pixel this screen occupies and did not write, which on the device shows
   * whatever was there before.
   *
   * That is exactly what shipped: the main screen's egg branch returned before
   * filling y=152..240, so the third starter row -- outline, sprite and the word
   * SQUIRTLE -- sat under the egg, and the band came back black from the pause
   * menu. render_at() cleared the framebuffer before every render, so this
   * harness measured each screen in a cleanroom the device never provides. */
  const uint16_t SENTINEL = 0xF81F; /* magenta; nothing in the palette uses it */
  int bleed_failures = 0;
  for (int b = 0; b < TAMAPOKE_SCREEN_COUNT; b++) {
    for (int a = 0; a < TAMAPOKE_SCREEN_COUNT; a++) {
      if (a == b) continue;

      render_at(a, 0);                  /* be on some other screen first */
      harness_fb_reset(SENTINEL);       /* every pixel is now "nobody drew here" */
      tamapoke_ui_goto_screen(b);
      tamapoke_input_reset(0);
      tamapoke_ui_render();

      const uint16_t *fb = harness_fb();
      int left = 0;
      for (int i = 0; i < FB_PIXELS; i++)
        if (fb[i] == SENTINEL) left++;

      if (left) {
        printf("FAIL %-14s leaves %d px unpainted arriving from %s -- they show "
               "the previous screen on hardware\n",
               tamapoke_ui_screen_name(b), left, tamapoke_ui_screen_name(a));
        bleed_failures++;
        break;                          /* one report per screen is enough */
      }
    }
  }
  if (!bleed_failures)
    printf("ok   no screen inherits pixels from the screen before it\n");
  failures += bleed_failures;

  int total = TAMAPOKE_SCREEN_COUNT * (int)(sizeof(LANGS) / sizeof(LANGS[0]))
              * (int)(sizeof(THEMES) / sizeof(THEMES[0]));
  printf("\n%d/%d screens clean, PPMs in %s\n", total - failures, total, out_dir);
  return failures ? 1 : 0;
}
