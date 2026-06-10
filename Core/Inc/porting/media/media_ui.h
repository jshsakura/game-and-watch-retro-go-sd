// Drawing layer for the Music app: shared primitives, the iPod-style now-playing
// screen, the info screen, the lyrics view and the always-on button-hint bar.
// Rendering only — input loops live in main_media.c.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "media_id3.h"
#include "media_lyrics.h"

// repeat modes
enum { REPEAT_OFF = 0, REPEAT_ALL = 1, REPEAT_ONE = 2 };

typedef struct {
    char        path[256];
    media_tags_t tags;
    const char *title;          // -> tags.title or the file name
    const char *artist;         // -> tags.artist or ""
    const char *album;          // -> tags.album or ""
    int   sec, total;           // elapsed / total seconds
    int   track_index;          // 0-based position in the playlist
    int   track_count;
    bool  paused, shuffle, favorite;
    int   repeat;               // REPEAT_*
    int   volume;               // 0..9
    long  file_size;            // bytes (info screen)
    float scrub;                // >=0 while seek-scrubbing (preview), else -1
} player_state_t;

// --- shared primitives (also used by the browser list) ---
void ui_fill(int x, int y, int w, int h, uint16_t c);
int  ui_text(int x, int y, int w, const char *t, uint16_t fg, uint16_t bg);
int  ui_text_t(int x, int y, int w, const char *t, uint16_t fg);   // transparent bg
void ui_text_center(int y, const char *t, uint16_t fg);            // over solid bg=current
void ui_text_center_t(int y, const char *t, uint16_t fg);          // transparent
void ui_text_bold_center_t(int y, const char *t, uint16_t fg);     // faux-bold, transparent
uint16_t ui_dim(uint16_t c, int num, int den);                     // c * num/den toward black

// --- now-playing ---
// Static layer (cover backdrop + card + title/artist): draw once per track into
// BOTH framebuffers. cover_n/cover_is_png come from cover_load().
void ui_player_static(const player_state_t *ps, int cover_n, bool cover_is_png);
// Dynamic layer (top bar + transport + hint bar): redraw every frame.
void ui_player_dynamic(const player_state_t *ps);

// --- browser list (handles large libraries: scrollbar, position, ellipsis) ---
enum { LIST_TRACK = 0, LIST_DIR = 1, LIST_SPECIAL = 2 };

typedef struct {
    const char     *title;       // primary line (id3 title or file name)
    const char     *subtitle;    // artist, or ""
    const char     *duration;    // "3:45", or ""
    const uint16_t *art;         // thumbnail pixels (art_sz×art_sz) or NULL
    int             art_sz;
    int             kind;        // LIST_*
    bool            fav;
} list_item_t;

typedef struct {
    const char *header;          // folder path or "★ 즐겨찾기"
    int  count;                  // total entries
    int  cursor, scroll;         // selection + first visible row
    int  visible_rows, row_h;
    bool busy;                   // true while fast-scrolling (skip art/sub)
} list_view_t;

#define LIST_HEADER_H 20
#define LIST_FOOTER_H 16
#define LIST_ROW_H    34
#define LIST_VISIBLE_ROWS ((240 - LIST_HEADER_H - LIST_FOOTER_H) / LIST_ROW_H)

// Draw the browser list. `item_at(i, out)` fills a row's data lazily (called
// only for visible rows) so the caller can keep its metadata cache.
void ui_list_draw(const list_view_t *v, void (*item_at)(int i, list_item_t *out));

// --- info screen (full tags + technical table) ---
void ui_info_draw(const player_state_t *ps);

// --- lyrics (model + parser in media_lyrics.h) ---
// Draw the lyrics view; top_line is the first visible line.
void ui_lyrics_draw(const player_state_t *ps, const lyrics_t *ly, int top_line, int active);
