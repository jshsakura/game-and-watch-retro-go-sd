/* Layout constants and UI entry points for the TamaPoke port.
 *
 * The upstream game was laid out for a 466x466 circular panel. Every screen
 * has been re-cut for a 320x240 rectangle, so the numbers below are NOT the
 * upstream numbers scaled by 0.5 -- they were chosen by eye for the new aspect
 * ratio (header/battery/streak pushed into the corners the circle never had,
 * the bottom panel made shallower, grids tightened).
 *
 * The button input layer (tamapoke_input.cpp) reads these by name to build
 * its hitbox tables, so this file is the single source of truth for both the
 * draw code and the focus walker. Move a button here and its hitbox moves
 * with it.
 *
 * Coordinate convention: X/Y are the top-left of the widget unless the name
 * says _CX/_CY (centre). W/H are the full size. Everything is in the 320x240
 * space defined by GFX_WIDTH/GFX_HEIGHT.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "tamapoke_gfx.h"     /* GFX_GLYPH_W for KB_NAME_X */
#include "tamapoke_input.h"   /* focus_set_t for current_focus_set */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Display + scene geometry                                           */
/* ------------------------------------------------------------------ */

/* Screen centre -- the upstream CX/CY was the centre of the circular panel
 * (233,233); most centring math still keys off this. */
#define TP_CX           160
#define TP_CY           120

/* The pet sprite. PET_GROUND is the y of the feet (the ground line the sprite
 * stands on); PET_CY is the vertical centre the sprite is drawn around.
 * HORIZON is the sky/ground boundary in the scene background. */
#define TP_PET_GROUND   150
#define TP_PET_CY       104
#define TP_HORIZON      100

/* Caress hitbox -- the area upstream treated as "touch the pet". Covers the
 * sprite's footprint with margin; the focus highlight is a darker ground
 * shadow rather than a rectangle (see drawPet). */
#define PET_ZONE_X      80
#define PET_ZONE_Y      20
#define PET_ZONE_W      160
#define PET_ZONE_H      130

/* ------------------------------------------------------------------ */
/* Header strip (name/level left, battery right, streak centre)       */
/* ------------------------------------------------------------------ */

#define HDR_NAME_X      4
#define HDR_NAME_Y      2
#define HDR_NAME_SIZE   1       /* 8x8 cell: 12 chars = 96px, fits */

#define HDR_BATT_X      284     /* 320 - 36, leaves 36px for the icon */
#define HDR_BATT_Y      2
#define HDR_BATT_W      32
#define HDR_BATT_H      12

#define HDR_STREAK_X    130
#define HDR_STREAK_Y    2

/* ------------------------------------------------------------------ */
/* Design tokens -- the whole port draws with these, not with numbers */
/* ------------------------------------------------------------------ */

/* Every screen in this port had invented its own look. Nine screens carried
 * five different ideas of what "selected" means -- the starter rows inverted and
 * took a ring, the action buttons inverted and took a pulsing ring, the feed
 * cells went white with a ring, the card's name strip drew two hairlines, and the
 * gallery, keyboard and clock filled with orange -- and radii ran 2, 4, 6, 8 and
 * 10 with no rule behind them. Each was defensible alone; together they read as
 * five half-finished screens rather than one game.
 *
 * So: one radius scale, one accent, one focus treatment (TP_ACCENT ring + accent
 * surface + white label, drawn by drawTile()), one frosted surface for anything
 * floating over the scene. A new screen picks from these; it does not invent. */
#define TP_R_XS             2     /* hairline chips: page dots, tiny badges */
#define TP_R_SM             4     /* keys, small buttons, bars */
#define TP_R_MD             8     /* action buttons, rows, pills */
#define TP_R_LG            12     /* floating cards and dialogs */

#define TP_ACCENT           UI_BAR_WARN   /* the one "you are here" colour */
#define TP_FOCUS_RING       3             /* thickness of the accent plate */

/* Frosted card over the scene: a soft shadow, a translucent fill, a bright rim.
 * The same three layers as the bottom panel, so a dialog and the panel look like
 * parts of one surface language. */
#define TP_CARD_ALPHA       224
#define TP_CARD_SHADOW_A     56
#define TP_CARD_SHADOW_OFF    3

/* ------------------------------------------------------------------ */
/* Bottom panel -- the frosted plate the bars and buttons sit on      */
/* ------------------------------------------------------------------ */

/* Upstream's panel is a translucent white plate over the scene, not a slab of
 * paint: the grass and water show faintly through it. This port filled the band
 * with an opaque dark slate instead, which cost more than fidelity -- the labels
 * are drawn in inkColor(), which is DARK in the day theme, so "FOOD/JOY/ENE/HYG"
 * were dark grey on dark navy and reported from hardware as unreadable.
 *
 * Blended, not filled (Gfx::blendRect). The scene has to be painted all the way
 * down for that to work; drawScene() now fills to GFX_HEIGHT for exactly this
 * reason (and it closes the 2px seam the old 150..152 gap left). */
#define PANEL_Y             152
#define PANEL_H             ((GFX_HEIGHT) - (PANEL_Y))
#define PANEL_ALPHA_DAY     184   /* ~72% white: bright enough for dark ink */
#define PANEL_ALPHA_NIGHT   208   /* night ink is light, so the plate stays dark */
#define PANEL_RIM_H         2     /* highlight along the top edge of the glass */

/* ------------------------------------------------------------------ */
/* Stat bars -- 2x2 grid, sits between scene and buttons              */
/* ------------------------------------------------------------------ */

/* Each bar: a 4-char label (size 1 = 32px) then a rounded-rect track.
 * Two columns of ~150px each, two rows of ~18px each. */
#define BAR_COL0_X      6
#define BAR_COL1_X      162
#define BAR_ROW0_Y      158
#define BAR_ROW1_Y      178
#define BAR_LABEL_GAP   34      /* label width + 2px */
#define BAR_W           104
#define BAR_H           12
#define BAR_R           3

/* ------------------------------------------------------------------ */
/* Action buttons -- 4 across the bottom row                          */
/* ------------------------------------------------------------------ */

/* Round-rect icon buttons. 32x32, evenly spaced with margins. */
#define BTN_W           32
#define BTN_H           32
#define BTN_HALF        16
#define BTN_R           8
#define BTN_ROW_Y       204     /* 204..236, 4px floor margin */

#define BTN0_X          24      /* feed   */
#define BTN1_X          104     /* play   */
#define BTN2_X          184     /* light  */
#define BTN3_X          264     /* bath   */

/* ------------------------------------------------------------------ */
/* CTAs -- the evolve/farewell/runaway prompt buttons                 */
/* ------------------------------------------------------------------ */

/* These overlay the scene when the pet is ready. Stacked vertically centred
 * on screen. The choice dialog uses these same boxes. */
#define EVO_BTN_W       140
#define EVO_BTN_H       30
#define EVO_BTN_X       ((TP_CX) - (EVO_BTN_W) / 2)   /* 90 */
#define EVO_BTN_Y       90

#define FAR_BTN_W       200
#define FAR_BTN_H       28
#define FAR_BTN_X       ((TP_CX) - (FAR_BTN_W) / 2)   /* 60 */
#define FAR_BTN_Y       128

#define RUN_BTN_W       200
#define RUN_BTN_H       28
#define RUN_BTN_X       ((TP_CX) - (RUN_BTN_W) / 2)
#define RUN_BTN_Y       128

/* Choice dialog: two stacked CTAs (evolve on top, farewell beneath). */
#define CHOICE_DIALOG_W 220
#define CHOICE_DIALOG_H 150
#define CHOICE_DIALOG_X ((TP_CX) - (CHOICE_DIALOG_W) / 2)  /* 50 */
#define CHOICE_DIALOG_Y 50

/* ------------------------------------------------------------------ */
/* Feed menu -- 4 icons in a strip (food + 3 berries + candy row)     */
/* ------------------------------------------------------------------ */

/* Card screen: the name strip at the top renames, and a TRAIN button starts the
 * training sack. Upstream puts the latter on the card's second page; this port's
 * card is one page, so both targets are here. */
#define CARD_NAME_H     16
#define CARD_TRAIN_W    120
#define CARD_TRAIN_H    22
#define CARD_TRAIN_X    ((TP_CX) - (CARD_TRAIN_W) / 2)
#define CARD_TRAIN_Y    140
/* The stat lines, as one block rather than three floating baselines. */
#define CARD_STATS_X    8
#define CARD_STATS_Y    84
#define CARD_STATS_W    ((GFX_WIDTH) - 2 * (CARD_STATS_X))
#define CARD_STATS_H    48
/* BACK, as a tile like every other pressable thing. The focus set below derives
 * from these, so the hitbox and the drawing cannot disagree. */
#define CARD_BACK_W     72
#define CARD_BACK_H     20
#define CARD_BACK_Y     ((GFX_HEIGHT) - (CARD_BACK_H) - 6)

#define FEED_MENU_W     200
#define FEED_MENU_H     36
#define FEED_MENU_X     ((TP_CX) - (FEED_MENU_W) / 2)  /* 110 */
/* Above the panel, not on top of it. At 150 the tray straddled PANEL_Y and sat
 * over the stat bars, so the icons read as part of the panel rather than as a menu
 * floating above it -- and once the panel became translucent the two surfaces were
 * visibly the same material at the same height. */
#define FEED_MENU_Y     ((PANEL_Y) - (FEED_MENU_H) - 6)
#define FEED_MENU_R     TP_R_LG
/* Five 24px cells inside a 200px plate with an 8px margin either side: the
 * remaining 184 - 5*24 = 64 px is split into four 16px gaps, so the pitch is 40.
 * At the old 48 the fifth item (candy) started at X+200 -- exactly the plate's
 * right edge -- and was drawn 24px outside the menu it belongs to. The onTap
 * hit-test derives from the same constant, so it followed the icons out. */
#define FEED_ICON_GAP   40      /* centre-to-centre; see the arithmetic above */
#define FEED_ICON0_X    (FEED_MENU_X + 8)
#define FEED_ICON_Y     (FEED_MENU_Y + 6)
#define FEED_ICON_SZ    24

/* ------------------------------------------------------------------ */
/* Release-confirm dialog                                             */
/* ------------------------------------------------------------------ */

#define CONFIRM_W       180
#define CONFIRM_H       90
#define CONFIRM_X       ((TP_CX) - (CONFIRM_W) / 2)   /* 70 */
#define CONFIRM_Y       ((TP_CY) - (CONFIRM_H) / 2)   /* 75 */
#define CONFIRM_R       10
#define CONFIRM_BTN_H   24
#define CONFIRM_BTN_W   70
#define CONFIRM_YES_X   ((TP_CX) - CONFIRM_BTN_W - 6)
#define CONFIRM_NO_X    ((TP_CX) + 6)
#define CONFIRM_BTN_Y   (CONFIRM_Y + CONFIRM_H - CONFIRM_BTN_H - 8)

/* ------------------------------------------------------------------ */
/* Starter select -- 3 rows + language pill                           */
/* ------------------------------------------------------------------ */

#define STARTER_ROW_X       40
#define STARTER_ROW_W       240
#define STARTER_ROW_Y0      40
#define STARTER_ROW_H       50
#define STARTER_ROW_GAP     8
/* Thickness of the ring around the focused row. Drawn as a filled plate behind
 * the row, not as N concentric drawRoundRect()s: those share one radius while
 * growing, so each ring cuts a different corner arc and the result is a notched
 * outline -- visible on hardware and reported as "the border looks wrong". */
#define STARTER_SEL_RING    3
/* The three rows end at 206, and the band below them was empty. It now carries
 * the key hint, which is what the screen was missing: a first-run player has no
 * way to know the D-pad walks the rows and A picks one. */
#define STARTER_HINT_Y      216

#define LANG_PILL_W         80
#define LANG_PILL_H         22
#define LANG_PILL_X         ((TP_CX) - (LANG_PILL_W) / 2)  /* 120 */
#define LANG_PILL_Y         206
#define LANG_PILL_R         6

/* ------------------------------------------------------------------ */
/* Gallery -- 4x4 grid of dex thumbnails (16 per page, 10 pages)      */
/* ------------------------------------------------------------------ */

#define GAL_COLS        4
#define GAL_ROWS        4
#define GAL_CELL        44
#define GAL_X           ((TP_CX) - (GAL_COLS * GAL_CELL) / 2)  /* 68 */
#define GAL_Y           28
#define GAL_GAP         2       /* inner border between cells */

/* Gallery detail view (one big sprite + name) */
#define GAL_DETAIL_CY       90
#define GAL_DETAIL_NAME_Y   150
#define GAL_DET_SPRITE_CY   GAL_DETAIL_CY
#define GAL_DET_HEAD_Y      GAL_DETAIL_NAME_Y
#define GAL_DET_BACK_Y      212

/* ------------------------------------------------------------------ */
/* On-screen keyboard -- 6 cols x 5 rows = 30 keys                    */
/* ------------------------------------------------------------------ */

#define KB_COLS         6
#define KB_ROWS         5
#define KB_W            44
#define KB_H            34
#define KB_X            ((TP_CX) - (KB_COLS * KB_W) / 2)  /* 28 */
#define KB_Y            70
#define KB_R            4
#define KB_TEXT_SIZE    2       /* 16x16 glyph in a 44x34 cell */
#define KB_KEYS         "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-"  /* 28 letters + . - = 30 */
#define KB_SPECIAL0     28      /* DEL key index */
#define KB_SPECIAL1     29      /* OK key index */

/* Title above the keyboard */
#define KB_TITLE_Y      6

/* Name preview box above the keyboard */
#define KB_NAME_W       240
#define KB_NAME_H       32
#define KB_NAME_X       ((TP_CX) - (KB_NAME_W) / 2)
#define KB_NAME_Y       28
#define KB_NAME_R       6

/* ------------------------------------------------------------------ */
/* Clock screen                                                       */
/* ------------------------------------------------------------------ */

#define CLOCK_TITLE_Y       6
#define CLOCK_TITLE_SIZE    2
#define CLOCK_TIME_Y        28
#define CLOCK_TIME_SIZE     3
#define CLOCK_BTN_W         50
#define CLOCK_BTN_H         24
#define CLOCK_BTN_GAP       8
#define CLOCK_BTN_Y         88
#define CLOCK_BTN_ROW_X     ((TP_CX) - (2 * CLOCK_BTN_W + CLOCK_BTN_GAP + 4) / 2)
#define CLOCK_HMINUS_X      CLOCK_BTN_ROW_X
#define CLOCK_HPLUS_X       (CLOCK_HMINUS_X + CLOCK_BTN_W + 4)
#define CLOCK_MMINUS_X      (CLOCK_HPLUS_X + CLOCK_BTN_W + CLOCK_BTN_GAP - 4)
#define CLOCK_MPLUS_X       (CLOCK_MMINUS_X + CLOCK_BTN_W + 4)
#define CLOCK_PAIR_LBL_Y    (CLOCK_BTN_Y + CLOCK_BTN_H + 2)
#define CLOCK_PILL_Y        130
#define CLOCK_PILL_H        20
#define CLOCK_SOUND_W       90
#define CLOCK_SOUND_X       16
#define CLOCK_LANG_W        90
#define CLOCK_LANG_X        ((GFX_WIDTH) - 16 - CLOCK_LANG_W)
#define CLOCK_OK_W          80
#define CLOCK_OK_H          26
#define CLOCK_OK_X          ((TP_CX) - (CLOCK_OK_W) / 2)
#define CLOCK_OK_Y          170
#define CLOCK_OK_SIZE       2
#define CLOCK_HINT_Y        212

/* ------------------------------------------------------------------ */
/* Minigames                                                          */
/* ------------------------------------------------------------------ */

/* The playfield's own background: this screen is dark in both themes, so
 * anything drawn on it has to be told the ink rather than ask inkColor() -- which
 * is how the score came to be drawn dark-on-dark. */
#define GAME_BG             0x10C5   /* #101828, matches UI_BG_NIGHT's family */

/* Ball keepy-up, redesigned as paddle (see stepGame/renderGame). */
#define GAME_PADDLE_W       40
#define GAME_PADDLE_H       4
#define GAME_PADDLE_Y       224
#define GAME_PADDLE_X_MIN   0
#define GAME_PADDLE_X_MAX   (GFX_WIDTH - GAME_PADDLE_W)
#define GAME_BALL_R         4
#define GAME_TOP            16      /* playfield top (below header) */
#define GAME_BOTTOM         (GAME_PADDLE_Y - 1)
#define GAME_SCORE_Y        2
#define GAME_SCORE_SIZE     2
#define GAME_RECORD_Y       18
#define GAME_LIVES_Y        4
#define GAME_LIVES_X0       (GFX_WIDTH - 40)
#define GAME_LIVES_DX       8
#define GAME_LIVES_R        3
#define GAME_OVER_LABEL_Y   80
#define GAME_OVER_LABEL_SZ  3
/* One UI tick is 33 ms (TAMAPOKE_FPS 30) and both the ball and the paddle move
 * once per tick -- so every number here is px per 1/30 s, and reading them that way
 * is the only way to see how fast this actually plays.
 *
 * It played in slow motion. The ball started at 1.0 px/frame vertically = 30 px/s,
 * against a 208 px playfield: SEVEN SECONDS for one descent. The paddle's 6 px/frame
 * took 1.5 s to cross the screen, which is what "unresponsive" was. Same physics,
 * numbers a hand can play against: a descent is ~2 s, the paddle crosses in under
 * a second, and it still moves ~3x faster than the ball falls. */
#define GAME_PADDLE_STEP    10      /* 300 px/s -- crosses the screen in ~0.9 s */
#define GAME_BALL_VY_START  3.4f    /* ~100 px/s: a 208 px descent in ~2 s */
#define GAME_BALL_VX_START  2.2f
#define GAME_LIVES          3       /* misses before the round ends */
#define GAME_OVER_MS        2600    /* how long the result stays up */
#define GAME_BALL_VX_MIN    1.0f    /* below this the ball drops in a column */
#define GAME_BALL_VX_MAX    4.5f
/* Two arrows and an A, shown for the first couple of seconds of a round. The
 * paddle is the only thing on this screen that reacts to a key and nothing said
 * so. */
#define GAME_HINT_MS        2500
#define GAME_HINT_Y         200

/* Sack (strength training): sandbag with rope/body/seam + hit meter. */
#define SACK_ROPE_TOP_Y     16
#define SACK_BODY_TOP_Y     48
#define SACK_BODY_W         70
#define SACK_BODY_H         90
#define SACK_BODY_X         ((TP_CX) - (SACK_BODY_W) / 2)
#define SACK_TAPER_H        16
#define SACK_SEAM_Y         (SACK_BODY_TOP_Y + (SACK_BODY_H / 2))
/* The lower half is four stacked rows -- counter, "hit fast", the key legend, the
 * time bar -- and adding the legend meant re-spacing all four rather than
 * squeezing it into the 4px between the hint and the bar. */
#define SACK_HIT_COUNTER_Y  150
#define SACK_HIT_COUNTER_SZ 3
#define SACK_HINT_Y         176
#define SACK_HINT_SIZE      1
#define SACK_KEY_HINT_Y     188
#define SACK_BAR_W          200
#define SACK_BAR_H          12
#define SACK_BAR_X          ((TP_CX) - (SACK_BAR_W) / 2)
#define SACK_BAR_Y          208
#define SACK_RESULT_Y       80
#define SACK_RESULT_SIZE    3
#define SACK_PET_CY         60
#define SACK_ROUND_MS       10000   /* one training round */
#define SACK_RESULT_MS      3000    /* how long the result stays up */
#define SACK_SHAKE_PX       7.0f    /* recoil amplitude of one hit */
#define SACK_SHAKE_MS       200

/* ------------------------------------------------------------------ */
/* UI entry points -- called by the porting main loop and input layer */
/* ------------------------------------------------------------------ */

/* Per-frame: step game logic + render the active screen. */
void tamapoke_ui_tick(uint32_t now_ms);

/* One-time init (called once after gfx->begin). */
void tamapoke_ui_init(void);

/* The full-screen render dispatcher (upstream render()). Public so a debug
 * hook can force a redraw. */
void tamapoke_render(void);
void tamapoke_ui_render(void);

/* Force the next render to repaint the whole screen instead of drawing
 * incrementally. Call it after something outside the UI has painted over the
 * canvas -- the launcher's pause menu is the case that matters. */
void tamapoke_ui_force_full_repaint(void);

/* Called by the input layer on any button edge: renews whichever timed overlay is
 * open, so one tuned for a tap does not expire while a cursor walks to it. */
void tamapoke_ui_note_input(void);

/* The feed menu's focus set, by identity -- used by the host harness. */
const focus_set_t *tamapoke_focus_set_feed(void);

/* Harness only: stop pinning a screen, and read which one the state selects. */
void tamapoke_ui_release_harness_screen(void);
int tamapoke_ui_current_screen(void);

/* True if input has happened since the last frame -- lets the porting main
 * loop tell a "framestep" from real activity. */
bool tamapoke_ui_had_activity(void);

/* ---- host harness API ----
 * These let the host harness put the UI into a known screen and render it,
 * without having to drive the full input/state machine. No-op on device. */
#define TAMAPOKE_SCREEN_COUNT 12
void tamapoke_ui_goto_screen(int id);
const char *tamapoke_ui_screen_name(int id);

/* Hit-testing -- bodies are upstream's, unchanged. Synthesised coordinates
 * come from tamapoke_input.cpp. */
void onTap(int16_t x, int16_t y);
void onSwipe(int dir);
void onSwipeV(int dir);
void onBack(void);

/* ---- what the keys mean on the current screen ----
 *
 * A focus cursor is the right stand-in for a finger on a menu and the wrong one
 * for a game. The ball minigame needs LEFT/RIGHT to be an axis, held, and the
 * training sack needs A to be a hit rather than a tap on a widget -- both of
 * which are the opposite of "walk the focus set". The input layer asks. */
typedef enum {
  TP_INPUT_UI = 0,   /* focus cursor stands in for the finger */
  TP_INPUT_PADDLE,   /* ball minigame: LEFT/RIGHT are held, not stepped */
  TP_INPUT_MASH,     /* training sack: A is a hit */
} tp_input_mode_t;

tp_input_mode_t tamapoke_ui_input_mode(void);

/* TP_INPUT_PADDLE: -1/0/+1, the direction currently HELD. Applied per tick so
 * the paddle's speed comes from the clock and not from how fast the main loop
 * happens to poll. */
void tamapoke_paddle_hold(int dir);

/* TP_INPUT_MASH: one hit on the sack. */
void tamapoke_sack_hit(void);

/* The two labelled keys on the console.
 *
 * The port used to reach every screen by walking the focus cursor off an edge:
 * the settings screen was "press DOWN on the main screen", the Pokedex was
 * "press RIGHT six times until you fall off the end of the button row". Both
 * work and neither is discoverable, and the second one fires by accident every
 * time someone overshoots the last button. TIME and GAME are printed on the
 * shell right next to the screen, so they get the two screens they name. */
void tamapoke_time_key(void);   /* TIME: the clock / settings screen */
void tamapoke_game_key(void);   /* GAME: the ball minigame */

/* UP / DOWN on the main screen. Their own entry points rather than a synthesised
 * vertical swipe: with TIME owning the settings screen, DOWN is free to open the
 * Pokedex -- which is what stops LEFT/RIGHT from having to eject the player off
 * the end of the row to get there. */
void tamapoke_open_card(void);
void tamapoke_open_gallery(void);

/* ---- harness probe ----
 * Game state a host test can assert on. Both minigames shipped as animations
 * with no way to play them and no reward when they ended, which no amount of
 * rendering could show; a test has to be able to read the score. */
typedef struct {
  int   screen;
  int   paddle_x;
  int   ball_x;         /* so a test can steer the paddle AWAY and force a miss */
  int   ball_y;
  uint8_t game_score;
  uint8_t game_misses;
  uint16_t sack_hits;
  uint8_t sack_gain;
  uint8_t pet_tr_atk;   /* trainStrength() writes this -- proof of the reward */
  uint8_t pet_tr_spe;   /* playResult() writes this */
} tamapoke_probe_t;

void tamapoke_ui_probe(tamapoke_probe_t *out);

/* Harness only: draw one Pokedex thumbnail, so a test can assert that the pack is
 * decoded the way the packer writes it. */
void tamapoke_ui_draw_thumb(int16_t dex, int16_t cx, int16_t cy, uint8_t s);

/* Focus/dim control -- supplied for tamapoke_input.cpp. */
const focus_set_t *tamapoke_current_focus_set(void);
bool tamapoke_is_dimmed(void);
void tamapoke_wake(void);
void tamapoke_hold_release(void);

#ifdef __cplusplus
}
#endif
