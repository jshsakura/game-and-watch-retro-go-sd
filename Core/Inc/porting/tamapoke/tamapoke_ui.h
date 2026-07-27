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

#define FEED_MENU_W     200
#define FEED_MENU_H     36
#define FEED_MENU_X     ((TP_CX) - (FEED_MENU_W) / 2)  /* 110 */
#define FEED_MENU_Y     150
#define FEED_MENU_R     8
#define FEED_ICON_GAP   48      /* centre-to-centre */
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

/* Sack (strength training): sandbag with rope/body/seam + hit meter. */
#define SACK_ROPE_TOP_Y     16
#define SACK_BODY_TOP_Y     48
#define SACK_BODY_W         70
#define SACK_BODY_H         90
#define SACK_BODY_X         ((TP_CX) - (SACK_BODY_W) / 2)
#define SACK_TAPER_H        16
#define SACK_SEAM_Y         (SACK_BODY_TOP_Y + (SACK_BODY_H / 2))
#define SACK_HIT_COUNTER_Y  152
#define SACK_HIT_COUNTER_SZ 3
#define SACK_HINT_Y         182
#define SACK_HINT_SIZE      1
#define SACK_BAR_W          200
#define SACK_BAR_H          12
#define SACK_BAR_X          ((TP_CX) - (SACK_BAR_W) / 2)
#define SACK_BAR_Y          200
#define SACK_RESULT_Y       80
#define SACK_RESULT_SIZE    3
#define SACK_PET_CY         60

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

/* True if input has happened since the last frame -- lets the porting main
 * loop tell a "framestep" from real activity. */
bool tamapoke_ui_had_activity(void);

/* ---- host harness API ----
 * These let the host harness put the UI into a known screen and render it,
 * without having to drive the full input/state machine. No-op on device. */
#define TAMAPOKE_SCREEN_COUNT 11
void tamapoke_ui_goto_screen(int id);
const char *tamapoke_ui_screen_name(int id);

/* Hit-testing -- bodies are upstream's, unchanged. Synthesised coordinates
 * come from tamapoke_input.cpp. */
void onTap(int16_t x, int16_t y);
void onSwipe(int dir);
void onSwipeV(int dir);
void onBack(void);

/* Focus/dim control -- supplied for tamapoke_input.cpp. */
const focus_set_t *tamapoke_current_focus_set(void);
bool tamapoke_is_dimmed(void);
void tamapoke_wake(void);
void tamapoke_hold_release(void);

#ifdef __cplusplus
}
#endif
