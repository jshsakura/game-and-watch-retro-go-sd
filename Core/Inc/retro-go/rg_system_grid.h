#pragma once

#include <stdbool.h>

/* System grid: every console on one screen, as icons.
 *
 * The launcher has 29 tabs and LEFT/RIGHT steps one at a time, so reaching the
 * far side costs up to 28 presses. Holding LEFT or RIGHT opens this grid, where
 * any system is at most 8 presses away.
 *
 * It costs no RAM and no new art: the cells are the 28x28 color_icon_t icons
 * already in internal flash (color_icon_for_logo), and the highlighted system is
 * named by the launcher's own header bar, which keeps drawing its wordmark.
 */

/* Cell grid. Each system sits on a rounded tile; the cell is the tile plus its
 * share of the gutter, so an 8px gap falls out of 50 - 42 in both axes.
 *
 * 6 x 50 = 300 of the 320px screen, and 3 x 50 = 150 of the 160px viewport —
 * the 10px left over vertically is RG_GRID_MARGIN_Y at top and bottom, which is
 * what keeps the top row's tile off the status bar. gui.tabs[] is capped at 32,
 * so six columns can never need more than six rows. */
#define RG_GRID_COLS     (6)
#define RG_GRID_CELL_W   (50)
#define RG_GRID_CELL_H   (50)
#define RG_GRID_TILE     (42) /* the rounded plate an icon sits on */
#define RG_GRID_RADIUS   (6)
#define RG_GRID_MARGIN_Y (5)  /* clearance from the status/header bars */
#define RG_GRID_SCREEN_W (320) /* asserted against ODROID_SCREEN_WIDTH in the .c */
#define RG_GRID_X0       ((RG_GRID_SCREEN_W - RG_GRID_COLS * RG_GRID_CELL_W) / 2)

/* ---- Layout: pure arithmetic, no globals and no LCD (rg_system_grid_layout.c).
 * Kept apart from the drawing so the host test can compile the real thing rather
 * than a re-implementation of it. ---- */

/** Rows needed to hold tab_count cells. */
int rg_grid_row_count(int tab_count);

/** Rows that fit in a viewport view_h pixels tall. */
int rg_grid_visible_rows(int view_h);

/** First row to display so that `cursor` is on screen, clamped to the last page. */
int rg_grid_scroll_row(int cursor, int first_row, int tab_count, int view_h);

/** Cursor after a step of (dx, dy) cells. Never leaves [0, tab_count). */
int rg_grid_move(int cursor, int dx, int dy, int tab_count);

/** Top-left of cell `index`. False when that cell is scrolled out of view. */
bool rg_grid_cell_rect(int index, int first_row, int view_y0, int view_h,
                       int *x, int *y);

/* ---- Screen (rg_system_grid.c) ---- */

bool rg_system_grid_is_open(void);

/** Open on the current tab. */
void rg_system_grid_open(void);

/** Close without changing tabs. */
void rg_system_grid_close(void);

/** Close and switch to the highlighted system. */
void rg_system_grid_commit(void);

/** Highlighted tab index (valid only while open). */
int rg_system_grid_cursor(void);

/** Move the highlight; (dx, dy) are in cells. */
void rg_system_grid_step(int dx, int dy);

/** Draw the grid into the list viewport. */
void rg_system_grid_draw(void);
