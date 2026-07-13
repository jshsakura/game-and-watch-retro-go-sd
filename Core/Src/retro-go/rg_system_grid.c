/* System grid — the launcher's console picker.
 *
 * Draws every tab as an icon inside the list viewport. The status bar and the
 * header bar keep drawing themselves (gui_redraw_callback feeds them the
 * highlighted tab), so the grid inherits the theme and names the highlighted
 * system with that system's own wordmark. No new art, no allocation.
 *
 * There is deliberately no loop in this file. The grid is a mode of retro_loop(),
 * not a screen of its own — a screen with its own loop is how the clock app ended
 * up never asking odroid_idle_timeout_expired() and sitting lit for ever
 * (see CLAUDE.md, "The bug is usually in the thing that never got wired").
 */

#include <odroid_system.h>

#include "main.h" /* get_darken_pixel() */
#include "gui.h"
#include "bitmaps.h"
#include "rg_system_grid.h"

_Static_assert(RG_GRID_SCREEN_W == ODROID_SCREEN_WIDTH,
               "grid layout was computed for a 320px screen");
_Static_assert(RG_GRID_X0 >= 0,
               "grid columns are wider than the screen");

/* The viewport the ROM list would have used. The grid never shows the subfolder
 * path strip, so unlike gui_draw_list() its viewport is always the full band. */
#define GRID_VIEW_Y0 (RG_STATUS_HEIGHT)
#define GRID_VIEW_H  (ODROID_SCREEN_HEIGHT - RG_STATUS_HEIGHT - RG_HEADER_HEIGHT)

static bool grid_open;
static int grid_cursor;    /* tab index */
static int grid_first_row; /* topmost visible row */

bool rg_system_grid_is_open(void)
{
    return grid_open;
}

int rg_system_grid_cursor(void)
{
    return grid_cursor;
}

void rg_system_grid_open(void)
{
    grid_cursor = gui.selected;
    grid_first_row = rg_grid_scroll_row(grid_cursor, 0, gui.tabcount, GRID_VIEW_H);
    grid_open = true;
}

void rg_system_grid_close(void)
{
    grid_open = false;
}

void rg_system_grid_commit(void)
{
    if (!grid_open)
        return;

    grid_open = false;

    if (grid_cursor != gui.selected)
        gui_set_current_tab(grid_cursor);
}

void rg_system_grid_step(int dx, int dy)
{
    if (!grid_open)
        return;

    grid_cursor = rg_grid_move(grid_cursor, dx, dy, gui.tabcount);
    grid_first_row = rg_grid_scroll_row(grid_cursor, grid_first_row, gui.tabcount,
                                        GRID_VIEW_H);
}

/* How far each row of the tile is inset from the tile's edge, top-down; the same
 * table mirrors to the bottom and to both sides. This IS the rounded corner —
 * there is no circle to rasterise at 6px, and a hand-picked profile reads
 * smoother than anything sqrt() would hand back at this size. */
static const uint8_t grid_corner[RG_GRID_RADIUS] = { 5, 3, 2, 1, 1, 0 };

static int grid_tile_inset(int row)
{
    if (row < 0 || row >= RG_GRID_TILE)
        return RG_GRID_TILE / 2; /* off the tile: treat as fully inset */
    if (row < RG_GRID_RADIUS)
        return grid_corner[row];
    if (RG_GRID_TILE - 1 - row < RG_GRID_RADIUS)
        return grid_corner[RG_GRID_TILE - 1 - row];

    return 0;
}

/* The rounded plate every system sits on. Only its two colours change when the
 * cursor lands on it, so the grid reads as one surface with one thing lit up. */
static void grid_draw_tile(int x, int y, bool selected)
{
    uint16_t fill = selected
        ? curr_colors->main_c
        : get_darken_pixel_d(curr_colors->main_c, curr_colors->bg_c, 22);
    uint16_t edge = selected
        ? curr_colors->sel_c
        : get_darken_pixel_d(curr_colors->dis_c, curr_colors->bg_c, 40);

    for (int row = 0; row < RG_GRID_TILE; row++)
    {
        int ins = grid_tile_inset(row);

        odroid_overlay_draw_fill_rect(x + ins, y + row, RG_GRID_TILE - 2 * ins, 1, fill);
    }

    /* Outline, following the same profile: the flat top and bottom rows are a
     * full span, the straight sides are one pixel, and each corner step is the
     * run between this row's inset and its neighbour's. */
    for (int row = 0; row < RG_GRID_TILE; row++)
    {
        int ins = grid_tile_inset(row);

        if (row == 0 || row == RG_GRID_TILE - 1)
        {
            odroid_overlay_draw_fill_rect(x + ins, y + row, RG_GRID_TILE - 2 * ins, 1, edge);
            continue;
        }

        odroid_overlay_draw_fill_rect(x + ins, y + row, 1, 1, edge);
        odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - 1 - ins, y + row, 1, 1, edge);

        int step = grid_tile_inset(row - 1);
        if (step > ins && row - 1 >= 0)
        {
            odroid_overlay_draw_fill_rect(x + ins, y + row, step - ins, 1, edge);
            odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - step, y + row, step - ins, 1, edge);
        }

        step = grid_tile_inset(row + 1);
        if (step > ins && row + 1 < RG_GRID_TILE)
        {
            odroid_overlay_draw_fill_rect(x + ins, y + row, step - ins, 1, edge);
            odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - step, y + row, step - ins, 1, edge);
        }
    }
}

static void grid_draw_scrollbar(void)
{
    int rows = rg_grid_row_count(gui.tabcount);
    int visible = rg_grid_visible_rows(GRID_VIEW_H);

    if (rows <= visible)
        return;

    int x = RG_GRID_X0 + RG_GRID_COLS * RG_GRID_CELL_W + 3;
    int track_y = GRID_VIEW_Y0 + RG_GRID_MARGIN_Y;
    int track_h = visible * RG_GRID_CELL_H;
    int thumb_h = track_h * visible / rows;
    int thumb_y = track_y + (track_h - thumb_h) * grid_first_row / (rows - visible);

    odroid_overlay_draw_fill_rect(x, track_y, 2, track_h,
                                  get_darken_pixel(curr_colors->dis_c, 50));
    odroid_overlay_draw_fill_rect(x, thumb_y, 2, thumb_h, curr_colors->sel_c);
}

void rg_system_grid_draw(void)
{
    odroid_overlay_draw_fill_rect(0, GRID_VIEW_Y0, ODROID_SCREEN_WIDTH, GRID_VIEW_H,
                                  curr_colors->bg_c);

    for (int i = 0; i < gui.tabcount; i++)
    {
        int x, y;

        if (!rg_grid_cell_rect(i, grid_first_row, GRID_VIEW_Y0, GRID_VIEW_H, &x, &y))
            continue;

        grid_draw_tile(x + (RG_GRID_CELL_W - RG_GRID_TILE) / 2,
                       y + (RG_GRID_CELL_H - RG_GRID_TILE) / 2,
                       i == grid_cursor);

        /* A tab with no colour art keeps its tile — an empty plate reads as
         * "no icon yet", an empty hole reads as a rendering bug. */
        const color_icon_t *icon = color_icon_for_logo(gui.tabs[i]->logo_idx);

        if (icon)
        {
            gui_draw_color_icon(x + (RG_GRID_CELL_W - icon->width) / 2,
                                y + (RG_GRID_CELL_H - icon->height) / 2, icon);
        }
    }

    grid_draw_scrollbar();
}
