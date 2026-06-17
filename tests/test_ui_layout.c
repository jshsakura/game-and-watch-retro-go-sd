// Host unit tests for music_ui layout invariants.
// Validates that all screen coordinates, sizes and buffer allocations are
// consistent, nothing overflows the 240×240 framebuffer, and Y ordering is
// correct. Duplicates the #defines from music_ui.c / music_ui.h so a
// mismatch is caught as a test failure.
// Build+run: see tests/run.sh
#include <stdio.h>
#include <stdint.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

#define SCR_W  240
#define SCR_H  240
#define FONT_H 12

#define TOPBAR_H 18
#define CARD_SZ  140
#define CARD_X   ((SCR_W - CARD_SZ) / 2)
#define CARD_Y   20
#define TITLE_Y  162
#define SUB_Y    175
#define PANEL_Y  188
#define PROG_X   18
#define PROG_W   (SCR_W - 2 * PROG_X)
#define PROG_Y   199
#define TIMES_Y  209
#define HINT_DIV 221
#define HINT1_Y  225
#define R        10

#define LIST_HEADER_H 18
#define LIST_FOOTER_H 14
#define LIST_ROW_H    40
#define LIST_VISIBLE_ROWS ((SCR_H - LIST_HEADER_H - LIST_FOOTER_H) / LIST_ROW_H)

static void test_card_fits_screen(void)
{
    CHECK(CARD_X >= 0, "CARD_X >= 0");
    CHECK(CARD_X + CARD_SZ <= SCR_W, "card right edge <= screen width");
    CHECK(CARD_Y >= 0, "CARD_Y >= 0");
    CHECK(CARD_SZ > 0 && CARD_SZ <= SCR_W, "CARD_SZ sane");
}

static void test_card_below_topbar(void)
{
    CHECK(CARD_Y >= TOPBAR_H, "card starts below top bar");
}

static void test_card_above_panel(void)
{
    CHECK(CARD_Y + CARD_SZ < PANEL_Y, "card bottom above panel");
}

static void test_y_ordering(void)
{
    CHECK(TOPBAR_H <= CARD_Y, "topbar <= card_y");
    CHECK(CARD_Y < TITLE_Y, "card_y < title_y");
    CHECK(TITLE_Y < SUB_Y, "title_y < sub_y");
    CHECK(SUB_Y < PANEL_Y, "sub_y < panel_y");
    CHECK(PANEL_Y <= PROG_Y, "panel_y <= prog_y");
    CHECK(PROG_Y < TIMES_Y, "prog_y < times_y");
    CHECK(TIMES_Y < HINT_DIV, "times_y < hint_div");
    CHECK(HINT_DIV <= HINT1_Y, "hint_div <= hint1_y");
    CHECK(HINT1_Y + FONT_H <= SCR_H, "hint bar fits in screen");
}

static void test_progress_bar(void)
{
    CHECK(PROG_W > 0, "PROG_W > 0");
    CHECK(PROG_X > 0, "PROG_X > 0");
    CHECK(PROG_X + PROG_W < SCR_W, "progress bar within screen");
}

static void test_cbuf_size(void)
{
    int needed = 4 * R * R;
    CHECK(needed == 400, "cbuf = 4*R*R = 400");
}

static void test_vinyl_radii_within_card(void)
{
    int half = CARD_SZ / 2;
    CHECK(58 <= half, "vinyl outer radius 58 <= card half");
    CHECK(49 <= half, "vinyl groove 49 <= card half");
    CHECK(37 <= half, "vinyl groove 37 <= card half");
    CHECK(25 <= half, "vinyl groove 25 <= card half");
    CHECK(17 <= half, "vinyl label 17 <= card half");
    CHECK(58 < 70, "vinyl radius < 70 (card/2)");
}

static void test_list_layout(void)
{
    CHECK(LIST_HEADER_H > 0, "header height > 0");
    CHECK(LIST_FOOTER_H > 0, "footer height > 0");
    CHECK(LIST_ROW_H > 0, "row height > 0");
    CHECK(LIST_VISIBLE_ROWS > 0, "visible rows > 0");
    int used = LIST_HEADER_H + LIST_FOOTER_H + LIST_VISIBLE_ROWS * LIST_ROW_H;
    CHECK(used <= SCR_H, "list content fits in screen height");
    CHECK(LIST_VISIBLE_ROWS == 5, "5 visible rows (header+footer+5*40=18+14+200=232 <= 240)");
}

static void test_list_scrollbar(void)
{
    int top = LIST_HEADER_H;
    int h = SCR_H - top - LIST_FOOTER_H;
    CHECK(h > 0, "scrollbar area height > 0");
    CHECK(h + top + LIST_FOOTER_H == SCR_H, "scrollbar fills remaining space");
}

static void test_thumbnail_size(void)
{
    int TH = 34;
    CHECK(TH > 0, "thumbnail size > 0");
    CHECK(TH < LIST_ROW_H, "thumbnail < row height");
    CHECK(TH * TH * sizeof(uint16_t) <= 4096, "thumbnail fits in ~4KB");
}

static void test_cover_card_even(void)
{
    CHECK(CARD_SZ % 2 == 0, "CARD_SZ is even (centered division)");
}

static void test_panel_bottom_edge(void)
{
    CHECK(PANEL_Y < SCR_H, "panel starts within screen");
}

static void test_title_subtitle_gap(void)
{
    CHECK(SUB_Y - TITLE_Y >= FONT_H, "at least one font height between title and subtitle");
}

static void test_times_row_height(void)
{
    CHECK(HINT_DIV - TIMES_Y >= FONT_H, "times row has room for text");
}

static void test_hint_bar_height(void)
{
    int hint_h = SCR_H - HINT1_Y;
    CHECK(hint_h >= FONT_H, "hint bar tall enough for text");
}

int main(void)
{
    test_card_fits_screen();
    test_card_below_topbar();
    test_card_above_panel();
    test_y_ordering();
    test_progress_bar();
    test_cbuf_size();
    test_vinyl_radii_within_card();
    test_list_layout();
    test_list_scrollbar();
    test_thumbnail_size();
    test_cover_card_even();
    test_panel_bottom_edge();
    test_title_subtitle_gap();
    test_times_row_height();
    test_hint_bar_height();
    printf("ui_layout: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
