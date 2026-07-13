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

#define GRID_ICON_FALLBACK_SIZE (28)

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

static void grid_draw_selection(int x, int y)
{
    odroid_overlay_draw_fill_rect(x + 1, y + 1, RG_GRID_CELL_W - 2,
                                  RG_GRID_CELL_H - 2, curr_colors->main_c);

    odroid_overlay_draw_fill_rect(x, y, RG_GRID_CELL_W, 1, curr_colors->sel_c);
    odroid_overlay_draw_fill_rect(x, y + RG_GRID_CELL_H - 1, RG_GRID_CELL_W, 1,
                                  curr_colors->sel_c);
    odroid_overlay_draw_fill_rect(x, y, 1, RG_GRID_CELL_H, curr_colors->sel_c);
    odroid_overlay_draw_fill_rect(x + RG_GRID_CELL_W - 1, y, 1, RG_GRID_CELL_H,
                                  curr_colors->sel_c);
}

static void grid_draw_scrollbar(void)
{
    int rows = rg_grid_row_count(gui.tabcount);
    int visible = rg_grid_visible_rows(GRID_VIEW_H);

    if (rows <= visible)
        return;

    int x = RG_GRID_X0 + RG_GRID_COLS * RG_GRID_CELL_W + 1;
    int track_y = GRID_VIEW_Y0 + 4;
    int track_h = GRID_VIEW_H - 8;
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

        if (i == grid_cursor)
            grid_draw_selection(x, y);

        const color_icon_t *icon = color_icon_for_logo(gui.tabs[i]->logo_idx);

        if (icon)
        {
            gui_draw_color_icon(x + (RG_GRID_CELL_W - icon->width) / 2,
                                y + (RG_GRID_CELL_H - icon->height) / 2, icon);
        }
        else
        {
            /* A tab with no colour art still has to occupy its cell — an empty
             * hole reads as a rendering bug, a plate reads as "no icon yet". */
            odroid_overlay_draw_fill_rect(
                x + (RG_GRID_CELL_W - GRID_ICON_FALLBACK_SIZE) / 2,
                y + (RG_GRID_CELL_H - GRID_ICON_FALLBACK_SIZE) / 2,
                GRID_ICON_FALLBACK_SIZE, GRID_ICON_FALLBACK_SIZE,
                curr_colors->dis_c);
        }
    }

    grid_draw_scrollbar();
}
