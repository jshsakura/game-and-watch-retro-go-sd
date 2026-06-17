// Fast browser list for the Video app.
//
// The full list (top bar + banner + CJK row titles + footer) is rendered ONCE
// via the shared ui_list_draw and cached as a framebuffer snapshot. Cursor moves
// then only re-blit the snapshot and overlay the selection highlight + a live
// position count — they never re-run text layout, so they never touch the
// SD-backed glyph path. The result is instant, YouTube-style scrolling even on a
// slow SD (the one-time render still pays the font cost, moves do not).
#pragma once

#include <stdbool.h>
#include "music_ui.h"   // list_view_t, list_item_t

// rebuild=true  -> re-render the list (call after a scroll or content change)
// rebuild=false -> reuse the snapshot, just move the selection (cheap)
void video_ui_list(list_view_t *v, void (*item_at)(int, list_item_t *), bool rebuild);

// Force a rebuild on the next call (content changed, or g_scratch — the snapshot
// store — was reused by the video decoder).
void video_ui_list_invalidate(void);
