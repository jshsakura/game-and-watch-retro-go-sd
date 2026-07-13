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

/* How the unselected tiles hold back. Every one of these numbers was picked on a
 * monitor, wrong, and then fixed on the device — which is dimmer than any screen
 * you tune colours on, and which people run with the backlight turned down.
 *
 * The plate stays quiet so the row reads as a frame rather than 18 competing
 * blocks. The RIM is what draws the tile, and at 28% it had simply disappeared.
 *
 * Two things that seemed like good ideas and were not, both removed rather than
 * left in at a token strength:
 *   - growing the selected tile 44 -> 48px. Four pixels on a 320x240 panel; nobody
 *     saw it.
 *   - CRT scanlines over the plates. They looked right in a mockup and cost
 *     legibility on the real LCD, which is the only place that counts.
 *
 * What is left carries the selection on its own: the plate lights up, the rim
 * turns gold, and every other icon fades toward the background. */
#define GRID_TILE_FILL_PCT  (14)
#define GRID_TILE_EDGE_PCT  (75)
#define GRID_ICON_FADE_PCT  (62) /* how much of an unselected icon's own colour survives */

/* The rounded plate a system sits on. The highlighted one is brighter and
 * gold-rimmed; every other one is a whisper.
 *
 * Everything between the corners is a plain rectangle, so it is drawn as one —
 * only the 2 * RG_GRID_RADIUS corner rows need a scanline each. That is ~40
 * fill_rect calls a tile instead of ~150, and the launcher repaints all 18 of
 * them every frame. */
static void grid_draw_tile(int x, int y, int tile, bool selected)
{
    const uint16_t fill = selected
        ? curr_colors->main_c
        : get_darken_pixel_d(curr_colors->main_c, curr_colors->bg_c, GRID_TILE_FILL_PCT);
    const uint16_t edge = selected
        ? curr_colors->sel_c
        : get_darken_pixel_d(curr_colors->dis_c, curr_colors->bg_c, GRID_TILE_EDGE_PCT);
    const int straight_y = y + RG_GRID_RADIUS;
    const int straight_h = tile - 2 * RG_GRID_RADIUS;

    /* Body: the full-width middle, plus one scanline per corner row. */
    odroid_overlay_draw_fill_rect(x, straight_y, tile, straight_h, fill);

    for (int row = 0; row < RG_GRID_RADIUS; row++)
    {
        int ins = rg_grid_tile_inset(row, tile);
        int w = tile - 2 * ins;

        odroid_overlay_draw_fill_rect(x + ins, y + row, w, 1, fill);
        odroid_overlay_draw_fill_rect(x + ins, y + tile - 1 - row, w, 1, fill);
    }

    /* Outline: the two straight sides, then the corner steps. A corner row's
     * edge run is everything between its own inset and its neighbour's — that
     * run is what makes the diagonal read as a curve instead of a staircase. */
    odroid_overlay_draw_fill_rect(x, straight_y, 1, straight_h, edge);
    odroid_overlay_draw_fill_rect(x + tile - 1, straight_y, 1, straight_h, edge);

    for (int row = 0; row < RG_GRID_RADIUS; row++)
    {
        int ins = rg_grid_tile_inset(row, tile);
        int run;

        if (row == 0)
        {
            /* The flat cap: one span across the whole top (and bottom) edge. */
            run = tile - 2 * ins;
        }
        else
        {
            /* Step out to where the row above starts. Insets only shrink going
             * inward, so this run is what bridges one corner row to the next; a
             * row whose neighbour has the same inset still owes its own pixel. */
            run = rg_grid_tile_inset(row - 1, tile) - ins;
            if (run < 1)
                run = 1;
        }

        int mirror = y + tile - 1 - row;

        odroid_overlay_draw_fill_rect(x + ins, y + row, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + tile - ins - run, y + row, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + ins, mirror, run, 1, edge);
        odroid_overlay_draw_fill_rect(x + tile - ins - run, mirror, run, 1, edge);
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

        bool selected = (i == grid_cursor);

        grid_draw_tile(x + (RG_GRID_CELL_W - RG_GRID_TILE) / 2,
                       y + (RG_GRID_CELL_H - RG_GRID_TILE) / 2,
                       RG_GRID_TILE, selected);

        /* A tab with no colour art keeps its bare plate — that reads as "no icon
         * yet", where an empty hole would read as a rendering bug. */
        const color_icon_t *icon = color_icon_for_logo(gui.tabs[i]->logo_idx);

        if (!icon)
            continue;

        int ix = x + (RG_GRID_CELL_W - icon->width) / 2;
        int iy = y + (RG_GRID_CELL_H - icon->height) / 2;

        if (selected)
            gui_draw_color_icon(ix, iy, icon);
        else
            gui_draw_color_icon_fade(ix, iy, icon, curr_colors->bg_c, GRID_ICON_FADE_PCT);
    }

    grid_draw_scrollbar();
}
