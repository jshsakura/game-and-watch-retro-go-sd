/* System-grid layout: links the real Core/Src/retro-go/rg_system_grid_layout.c.
 *
 * The one invariant that matters is the last test here: whatever the cursor does,
 * the cell it lands on must be on screen. A grid that scrolls a hair too little
 * puts the selection under the header bar, where the user is pressing DOWN at a
 * highlight they cannot see. */

#include <stdio.h>
#include <string.h>

#include "rg_system_grid.h"

/* The launcher's list viewport: 240 tall, minus the status bar and header bar. */
#define VIEW_Y0 (33)
#define VIEW_H  (240 - 33 - 47)

#define TABS (29) /* 28 systems + the favorites tab, as emulators_init() builds it */

static int failures;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                          \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* The tile is what the user actually sees; the cell is just its slot. If the tile
 * ever outgrows the cell the gutters vanish and the grid goes back to looking
 * like the cramped thing this replaced. */
static void test_tile_fits_its_cell(void)
{
    CHECK(RG_GRID_TILE < RG_GRID_CELL_W && RG_GRID_TILE < RG_GRID_CELL_H,
          "the tile must leave a gutter: tile %d, cell %dx%d",
          RG_GRID_TILE, RG_GRID_CELL_W, RG_GRID_CELL_H);
    CHECK(2 * RG_GRID_RADIUS <= RG_GRID_TILE,
          "the corner radius cannot exceed half the tile");
    /* The gutters need not match — the screen is wider than the viewport is tall,
     * so the columns get more room than the rows. They do both need to exist, and
     * to be even, or the tile does not centre on a whole pixel. */
    CHECK(RG_GRID_CELL_W - RG_GRID_TILE >= 4,
          "tiles would nearly touch across: gutter %d", RG_GRID_CELL_W - RG_GRID_TILE);
    CHECK(RG_GRID_CELL_H - RG_GRID_TILE >= 4,
          "tiles would nearly touch down: gutter %d", RG_GRID_CELL_H - RG_GRID_TILE);
    CHECK((RG_GRID_CELL_W - RG_GRID_TILE) % 2 == 0 &&
          (RG_GRID_CELL_H - RG_GRID_TILE) % 2 == 0,
          "an odd gutter puts the tile half a pixel off centre");
    CHECK(RG_GRID_TILE >= 28, "a 28x28 icon must fit on the tile, tile is %d", RG_GRID_TILE);
}

/* The tile rasteriser is cheap because it trusts the corner profile: it draws the
 * middle as ONE rectangle and each corner row's outline as a single run out to
 * where the row above started. Both shortcuts are only valid while the profile
 * shrinks monotonically inward and is symmetric top-to-bottom. Edit the corner
 * table into something that bulges and the outline silently grows holes — on a
 * screen nobody diffs. So the property gets asserted, not the pixels. */
static void test_corner_profile(void)
{
    CHECK(rg_grid_tile_inset(0, RG_GRID_TILE) > 0, "the top row must be inset, or there is no corner");
    CHECK(rg_grid_tile_inset(RG_GRID_RADIUS - 1, RG_GRID_TILE) == 0,
          "the corner must reach the tile edge by the last radius row, got %d",
          rg_grid_tile_inset(RG_GRID_RADIUS - 1, RG_GRID_TILE));

    for (int row = 1; row < RG_GRID_RADIUS; row++)
    {
        CHECK(rg_grid_tile_inset(row, RG_GRID_TILE) <= rg_grid_tile_inset(row - 1, RG_GRID_TILE),
              "the corner bulges back out at row %d (%d after %d) — the outline "
              "rasteriser would leave a hole",
              row, rg_grid_tile_inset(row, RG_GRID_TILE), rg_grid_tile_inset(row - 1, RG_GRID_TILE));
    }

    for (int row = 0; row < RG_GRID_TILE; row++)
    {
        CHECK(rg_grid_tile_inset(row, RG_GRID_TILE) == rg_grid_tile_inset(RG_GRID_TILE - 1 - row, RG_GRID_TILE),
              "the tile must be symmetric top-to-bottom; row %d differs", row);
        CHECK(2 * rg_grid_tile_inset(row, RG_GRID_TILE) < RG_GRID_TILE,
              "row %d is inset past the tile's own middle", row);
    }

    /* The straight middle, which is drawn as a single rectangle. */
    for (int row = RG_GRID_RADIUS; row < RG_GRID_TILE - RG_GRID_RADIUS; row++)
    {
        CHECK(rg_grid_tile_inset(row, RG_GRID_TILE) == 0,
              "row %d is inside the straight band but reports inset %d",
              row, rg_grid_tile_inset(row, RG_GRID_TILE));
    }
}

static void test_row_count(void)
{
    CHECK(rg_grid_row_count(0) == 0, "empty grid has no rows");
    CHECK(rg_grid_row_count(1) == 1, "one tab is one row");
    CHECK(rg_grid_row_count(RG_GRID_COLS) == 1, "a full row is one row");
    CHECK(rg_grid_row_count(RG_GRID_COLS + 1) == 2, "one over spills to a 2nd row");
    CHECK(rg_grid_row_count(TABS) == 5, "29 tabs need 5 rows, got %d",
          rg_grid_row_count(TABS));
    CHECK(rg_grid_row_count(32) == 6, "the gui.tabs[] cap still fits 6 rows");
}

static void test_visible_rows(void)
{
    /* 160px viewport, 5px of clearance at each end, 50px rows -> 3. Reserving the
     * margin first is the whole point: a 4th row would only fit by sitting flush
     * against the status bar, which is exactly the look this replaced. */
    CHECK(rg_grid_visible_rows(VIEW_H) == 3, "3 rows of 50px fit the 160px viewport, got %d",
          rg_grid_visible_rows(VIEW_H));
    CHECK(rg_grid_visible_rows(0) >= 1, "a degenerate viewport must not divide by zero rows");
}

static void test_cells_stay_on_screen(void)
{
    for (int i = 0; i < TABS; i++)
    {
        for (int first = 0; first <= 3; first++)
        {
            int x = -1, y = -1;

            if (!rg_grid_cell_rect(i, first, VIEW_Y0, VIEW_H, &x, &y))
                continue;

            CHECK(x >= 0 && x + RG_GRID_CELL_W <= RG_GRID_SCREEN_W,
                  "cell %d (first_row %d) runs off the side: x=%d", i, first, x);
            CHECK(y >= VIEW_Y0 + RG_GRID_MARGIN_Y &&
                  y + RG_GRID_CELL_H <= VIEW_Y0 + VIEW_H - RG_GRID_MARGIN_Y,
                  "cell %d (first_row %d) crowds a bar: y=%d", i, first, y);
        }
    }
}

static void test_cell_positions(void)
{
    int x = 0, y = 0;

    CHECK(rg_grid_cell_rect(0, 0, VIEW_Y0, VIEW_H, &x, &y), "cell 0 is visible");
    CHECK(x == RG_GRID_X0, "cell 0 sits at the left margin, got x=%d", x);
    /* The complaint that produced this layout was "it's stuck to the bars". The
     * clearance is what fixed it, so the clearance is what gets asserted. */
    CHECK(y - VIEW_Y0 >= RG_GRID_MARGIN_Y,
          "the top row must clear the status bar by %d px, got %d",
          RG_GRID_MARGIN_Y, y - VIEW_Y0);

    CHECK(rg_grid_cell_rect(RG_GRID_COLS, 0, VIEW_Y0, VIEW_H, &x, &y), "cell 6 is visible");
    CHECK(x == RG_GRID_X0 && y == VIEW_Y0 + RG_GRID_MARGIN_Y + RG_GRID_CELL_H,
          "cell 6 starts the 2nd row, got (%d,%d)", x, y);

    /* Row 3 is below a 3-row viewport until the grid scrolls. */
    CHECK(!rg_grid_cell_rect(18, 0, VIEW_Y0, VIEW_H, &x, &y),
          "row 3 must be off-screen at first_row 0");
    CHECK(rg_grid_cell_rect(18, 1, VIEW_Y0, VIEW_H, &x, &y),
          "row 3 must be visible at first_row 1");
    CHECK(y >= VIEW_Y0 + RG_GRID_MARGIN_Y + 2 * RG_GRID_CELL_H,
          "scrolled row 3 lands on the last slot, y=%d", y);

    /* And the bottom row must clear the header bar by the same amount. */
    int last_visible = 2 * RG_GRID_COLS; /* first cell of the 3rd visible row */
    CHECK(rg_grid_cell_rect(last_visible, 0, VIEW_Y0, VIEW_H, &x, &y),
          "the 3rd row is on screen");
    CHECK((VIEW_Y0 + VIEW_H) - (y + RG_GRID_CELL_H) >= RG_GRID_MARGIN_Y,
          "the bottom row must clear the header bar by %d px, got %d",
          RG_GRID_MARGIN_Y, (VIEW_Y0 + VIEW_H) - (y + RG_GRID_CELL_H));
}

static void test_move_bounds(void)
{
    CHECK(rg_grid_move(0, -1, 0, TABS) == 0, "LEFT at the first cell stays put (no wrap)");
    CHECK(rg_grid_move(TABS - 1, +1, 0, TABS) == TABS - 1,
          "RIGHT at the last cell stays put (no wrap)");
    CHECK(rg_grid_move(3, 0, -1, TABS) == 3, "UP from the first row stays put");
    CHECK(rg_grid_move(0, +1, 0, TABS) == 1, "RIGHT steps one cell");
    CHECK(rg_grid_move(0, 0, +1, TABS) == RG_GRID_COLS, "DOWN steps one row");

    /* 29 tabs: row 4 holds indices 24..28. DOWN from 23 has no cell below it, but
     * refusing to move there would strand the user one row above the last system. */
    CHECK(rg_grid_move(23, 0, +1, TABS) == TABS - 1,
          "DOWN into the ragged last row lands on the last system, got %d",
          rg_grid_move(23, 0, +1, TABS));
    CHECK(rg_grid_move(26, 0, +1, TABS) == 26, "DOWN from the last row stays put");

    CHECK(rg_grid_move(0, 0, 0, 0) == 0, "an empty grid cannot move anywhere");
}

/* The invariant: after any sequence of moves, the cursor's cell is on screen. */
static void test_cursor_is_always_visible(void)
{
    static const int steps[][2] = {
        {+1, 0}, {+1, 0}, {0, +1}, {+1, 0}, {0, +1}, {0, +1}, {0, +1}, {0, +1},
        {-1, 0}, {0, -1}, {0, -1}, {+1, 0}, {0, +1}, {0, +1}, {0, +1}, {-1, 0},
        {0, -1}, {0, -1}, {0, -1}, {0, -1}, {+1, 0}, {0, +1},
    };
    int cursor = 0;
    int first = 0;

    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++)
    {
        cursor = rg_grid_move(cursor, steps[s][0], steps[s][1], TABS);
        first = rg_grid_scroll_row(cursor, first, TABS, VIEW_H);

        CHECK(cursor >= 0 && cursor < TABS, "step %zu: cursor %d left the tab range", s, cursor);
        CHECK(rg_grid_cell_rect(cursor, first, VIEW_Y0, VIEW_H, NULL, NULL),
              "step %zu: cursor %d is scrolled out of view (first_row %d)", s, cursor, first);
        CHECK(first >= 0 && first <= rg_grid_row_count(TABS) - rg_grid_visible_rows(VIEW_H),
              "step %zu: first_row %d scrolled past the last page", s, first);
    }
}

/* Opening the grid on any tab must show that tab. */
static void test_open_on_any_tab(void)
{
    for (int cursor = 0; cursor < TABS; cursor++)
    {
        int first = rg_grid_scroll_row(cursor, 0, TABS, VIEW_H);

        CHECK(rg_grid_cell_rect(cursor, first, VIEW_Y0, VIEW_H, NULL, NULL),
              "opening on tab %d must scroll it into view (first_row %d)", cursor, first);
    }
}

int main(void)
{
    test_tile_fits_its_cell();
    test_corner_profile();
    test_row_count();
    test_visible_rows();
    test_cells_stay_on_screen();
    test_cell_positions();
    test_move_bounds();
    test_cursor_is_always_visible();
    test_open_on_any_tab();

    if (failures)
    {
        printf("FAIL test_system_grid: %d check(s) failed\n", failures);
        return 1;
    }

    printf("OK  test_system_grid: layout, scrolling and cursor bounds\n");
    return 0;
}
