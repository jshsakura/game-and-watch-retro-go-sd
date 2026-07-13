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

/* The scrollbar lives in the strip between the last column and the screen edge.
 * Widening the cells eats that strip, so the compiler gets to check it. */
#define GRID_BAR_W 2
#define GRID_BAR_X (RG_GRID_X0 + RG_GRID_COLS * RG_GRID_CELL_W + 1)

_Static_assert(RG_GRID_SCREEN_W == ODROID_SCREEN_WIDTH,
               "grid layout was computed for a 320px screen");
_Static_assert(RG_GRID_X0 >= 0,
               "grid columns are wider than the screen");
_Static_assert(GRID_BAR_X + GRID_BAR_W <= RG_GRID_SCREEN_W,
               "no room left for the scrollbar beside the last column");

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

/* The rounded plate every system sits on. Only its two colours change when the
 * cursor lands on it, so the grid reads as one surface with one thing lit up.
 *
 * Everything between the corners is a plain rectangle, so it is drawn as one —
 * only the 2 * RG_GRID_RADIUS corner rows need a scanline each. That is ~40
 * fill_rect calls a tile instead of ~150, and the launcher repaints all 18 of
 * them every frame. */
static void grid_draw_tile(int x, int y, bool selected)
{
    const uint16_t fill = selected
        ? curr_colors->main_c
        : get_darken_pixel_d(curr_colors->main_c, curr_colors->bg_c, 22);
    const uint16_t edge = selected
        ? curr_colors->sel_c
        : get_darken_pixel_d(curr_colors->dis_c, curr_colors->bg_c, 40);
    const int straight_y = y + RG_GRID_RADIUS;
    const int straight_h = RG_GRID_TILE - 2 * RG_GRID_RADIUS;

    /* Body: the full-width middle, plus one scanline per corner row. */
    odroid_overlay_draw_fill_rect(x, straight_y, RG_GRID_TILE, straight_h, fill);

    for (int row = 0; row < RG_GRID_RADIUS; row++)
    {
        int ins = rg_grid_tile_inset(row);
        int w = RG_GRID_TILE - 2 * ins;

        odroid_overlay_draw_fill_rect(x + ins, y + row, w, 1, fill);
        odroid_overlay_draw_fill_rect(x + ins, y + RG_GRID_TILE - 1 - row, w, 1, fill);
    }

    /* Outline: the two straight sides, then the corner steps. A corner row's
     * edge run is everything between its own inset and its neighbour's — that
     * run is what makes the diagonal read as a curve instead of a staircase. */
    odroid_overlay_draw_fill_rect(x, straight_y, 1, straight_h, edge);
    odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - 1, straight_y, 1, straight_h, edge);

    for (int row = 0; row < RG_GRID_RADIUS; row++)
    {
        int ins = rg_grid_tile_inset(row);
        int run;

        if (row == 0)
        {
            /* The flat cap: one span across the whole top (and bottom) edge. */
            run = RG_GRID_TILE - 2 * ins;
        }
        else
        {
            /* Step out to where the row above starts. Insets only shrink going
             * inward, so this run is what bridges one corner row to the next; a
             * row whose neighbour has the same inset still owes its own pixel. */
            run = rg_grid_tile_inset(row - 1) - ins;
            if (run < 1)
                run = 1;
        }

        int mirror = y + RG_GRID_TILE - 1 - row;

        odroid_overlay_draw_fill_rect(x + ins, y + row, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - ins - run, y + row, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + ins, mirror, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + RG_GRID_TILE - ins - run, mirror, run, 1, edge);
    }
}

static void grid_draw_scrollbar(void)
{
    int rows = rg_grid_row_count(gui.tabcount);
    int visible = rg_grid_visible_rows(GRID_VIEW_H);

    if (rows <= visible)
        return;

    int track_y = GRID_VIEW_Y0 + RG_GRID_MARGIN_Y;
    int track_h = visible * RG_GRID_CELL_H;
    int thumb_h = track_h * visible / rows;
    int thumb_y = track_y + (track_h - thumb_h) * grid_first_row / (rows - visible);

    odroid_overlay_draw_fill_rect(GRID_BAR_X, track_y, GRID_BAR_W, track_h,
                                  get_darken_pixel(curr_colors->dis_c, 50));
    odroid_overlay_draw_fill_rect(GRID_BAR_X, thumb_y, GRID_BAR_W, thumb_h,
                                  curr_colors->sel_c);
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
