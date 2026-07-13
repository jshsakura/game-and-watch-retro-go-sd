/* System grid layout — pure arithmetic.
 *
 * Deliberately free of every launcher dependency (no gui.h, no LCD, no globals)
 * so tests/test_system_grid_layout.c can compile and link THIS file rather than
 * a copy of its rules. See CLAUDE.md: a test that reimplements what it tests
 * proves nothing.
 */

#include "rg_system_grid.h"

int rg_grid_row_count(int tab_count)
{
    if (tab_count <= 0)
        return 0;

    return (tab_count + RG_GRID_COLS - 1) / RG_GRID_COLS;
}

int rg_grid_visible_rows(int view_h)
{
    /* The margin is reserved first: a row that would only fit by sitting flush
     * against the status bar is a row we do not show. */
    int rows = (view_h - 2 * RG_GRID_MARGIN_Y) / RG_GRID_CELL_H;

    return (rows > 0) ? rows : 1;
}

int rg_grid_scroll_row(int cursor, int first_row, int tab_count, int view_h)
{
    int visible = rg_grid_visible_rows(view_h);
    int rows = rg_grid_row_count(tab_count);
    int cursor_row = (cursor >= 0) ? cursor / RG_GRID_COLS : 0;
    int last_first = rows - visible;

    if (last_first < 0)
        last_first = 0;

    /* Scroll only as far as it takes to bring the cursor's row back on screen. */
    if (cursor_row < first_row)
        first_row = cursor_row;
    else if (cursor_row >= first_row + visible)
        first_row = cursor_row - visible + 1;

    if (first_row > last_first)
        first_row = last_first;
    if (first_row < 0)
        first_row = 0;

    return first_row;
}

int rg_grid_move(int cursor, int dx, int dy, int tab_count)
{
    if (tab_count <= 0)
        return 0;

    if (cursor < 0)
        cursor = 0;
    else if (cursor >= tab_count)
        cursor = tab_count - 1;

    if (dx != 0)
    {
        int next = cursor + dx;

        /* Walks the whole grid in reading order and stops at both ends: wrapping
         * would jump the cursor across the screen, which reads as a glitch. */
        return (next >= 0 && next < tab_count) ? next : cursor;
    }

    if (dy != 0)
    {
        int next = cursor + dy * RG_GRID_COLS;

        if (next < 0)
            return cursor;

        /* Down out of the last, partly filled row lands on the last system
         * rather than refusing to move. */
        if (next >= tab_count)
        {
            int last_row = (tab_count - 1) / RG_GRID_COLS;

            if (dy > 0 && cursor / RG_GRID_COLS < last_row)
                return tab_count - 1;

            return cursor;
        }

        return next;
    }

    return cursor;
}

bool rg_grid_cell_rect(int index, int first_row, int view_y0, int view_h,
                       int *x, int *y)
{
    int row = index / RG_GRID_COLS - first_row;
    int visible = rg_grid_visible_rows(view_h);

    if (index < 0 || row < 0 || row >= visible)
        return false;

    /* Centre the whole block of rows in the viewport, so a short grid does not
     * hug the status bar. */
    int y_pad = (view_h - visible * RG_GRID_CELL_H) / 2;

    if (x)
        *x = RG_GRID_X0 + (index % RG_GRID_COLS) * RG_GRID_CELL_W;
    if (y)
        *y = view_y0 + y_pad + row * RG_GRID_CELL_H;

    return true;
}
