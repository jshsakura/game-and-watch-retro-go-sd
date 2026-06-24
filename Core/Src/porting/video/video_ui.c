// Fast browser list — see video_ui.h.
//
// One full render is snapshotted into g_scratch (the Music cover scratch, free
// while browsing). Each cursor move re-blits that snapshot and paints only the
// selection highlight + a live position count, so moves never re-run text layout
// (the slow, SD-backed glyph path). The list look stays pixel-identical to Music
// because the one-time render still goes through the shared ui_list_draw.

#include "video_ui.h"
#include "gw_lcd.h"
#include "gui.h"               // curr_colors
#include "rg_i18n.h"           // i18n_get_text_width
#include "music_cover.h"       // g_scratch (snapshot store)
#include <string.h>
#include <stdio.h>

#define FB_PX (GW_LCD_WIDTH * GW_LCD_HEIGHT)

static bool s_valid = false;

void video_ui_list_invalidate(void) { s_valid = false; }

// Blend RGB565 a toward b by n/16.
static inline uint16_t vmix(uint16_t a, uint16_t b, int n)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (((br - ar) * n) >> 4);
    int g = ag + (((bg - ag) * n) >> 4);
    int c = ab + (((bb - ab) * n) >> 4);
    return (uint16_t)((r << 11) | (g << 5) | c);
}

void video_ui_list(list_view_t *v, void (*item_at)(int, list_item_t *), bool rebuild)
{
    uint16_t *snap = (uint16_t *)g_scratch;

    if (rebuild || !s_valid) {
        int cur = v->cursor;
        v->cursor = -1;                 // render clean: no row highlighted, "0/N" count
        ui_list_draw(v, item_at);
        v->cursor = cur;
        memcpy(snap, lcd_get_active_buffer(), FB_PX * sizeof(uint16_t));
        s_valid = true;
    }

    uint16_t *fb = lcd_get_active_buffer();
    memcpy(fb, snap, FB_PX * sizeof(uint16_t));

    if (v->count > 0) {
        uint16_t accent = curr_colors->sel_c;

        // selection highlight over the cached row (tint keeps the text readable).
        int vis = v->cursor - v->scroll;
        if (vis >= 0 && vis < v->visible_rows) {
            int y0 = LIST_HEADER_H + LIST_BANNER_H + vis * v->row_h;
            for (int yy = 0; yy < v->row_h; yy++) {
                int y = y0 + yy;
                if (y < 0 || y >= GW_LCD_HEIGHT) continue;
                uint16_t *row = fb + y * GW_LCD_WIDTH;
                bool edge = (yy == 0 || yy == v->row_h - 1);
                for (int x = 0; x < GW_LCD_WIDTH; x++)
                    row[x] = edge ? vmix(row[x], accent, 11) : vmix(row[x], accent, 4);
            }
        }

        // live "cursor/total" in the footer (ASCII -> baked font, no SD). Cover the
        // snapshot's stale "0/N" with the footer surface colour sampled from itself.
        int fy0 = GW_LCD_HEIGHT - LIST_FOOTER_H;
        uint16_t surf = fb[(fy0 + LIST_FOOTER_H / 2) * GW_LCD_WIDTH + (GW_LCD_WIDTH - 70)];
        char pos[24];
        snprintf(pos, sizeof pos, "%d/%d", v->cursor + 1, v->count);
        int pw = i18n_get_text_width(pos);
        ui_fill(GW_LCD_WIDTH - pw - 14, fy0 + 2, pw + 12, LIST_FOOTER_H - 3, surf);
        ui_text_t(GW_LCD_WIDTH - pw - 8, fy0 + (LIST_FOOTER_H - 12) / 2, pw + 2, pos,
                  curr_colors->dis_c);
    }

    lcd_swap();
}
