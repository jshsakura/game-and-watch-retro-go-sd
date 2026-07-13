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
 * share of the gutter, so the gap between tiles falls out of CELL - TILE: 8px
 * across, 6px down.
 *
 * 6 x 52 = 312 of the 320px screen, which leaves the tiles 8px clear of the
 * side edges — wide cells rather than a wide margin, so the row does not float
 * in the middle of the screen. Vertically 3 x 50 = 150 of the 160px viewport,
 * and the 10px left over is RG_GRID_MARGIN_Y at top and bottom, which is what
 * keeps the tiles off the status and header bars.
 *
 * gui.tabs[] is capped at 32, so six columns can never need more than six rows. */
#define RG_GRID_COLS     (6)
#define RG_GRID_CELL_W   (52)
#define RG_GRID_CELL_H   (50)
#define RG_GRID_TILE     (44) /* the rounded plate an icon sits on */
#define RG_GRID_TILE_SEL (48) /* the highlighted one lifts, rather than only recolouring */
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

/** How far row `row` of a `tile`-pixel tile is inset from its edge — this IS the
 * rounded corner. One profile serves both tile sizes; it only ever touches the
 * outermost RG_GRID_RADIUS rows. Rows outside the tile report a full inset.
 *
 * The profile must be non-increasing across the corner (row 0 is the most
 * inset). The tile rasteriser leans on that: it draws each corner row's edge as
 * a single run out to where the row above began, and the straight middle as one
 * rectangle. A profile that bulged back out would leave holes in the outline. */
int rg_grid_tile_inset(int row, int tile);

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
