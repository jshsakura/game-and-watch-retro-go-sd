---
slug: md32x-menu-flicker-the-memcpy-that-copied-ghosts
title: "The menu that flickered — a memcpy that copied the previous frame's UI forever"
authors: [jshsakura]
tags: [32x, fault, hardware]
image: /img/clock-hero.jpg
---

One of the strangest-looking bugs on the 32X port was the pause menu. Open it,
and for one frame everything was fine — the game frame behind a dimmed overlay,
the dialog on top. By the third frame the game frame was gone, replaced by a
black void, and the **previous frame's UI was smearing itself on top of the new
one**, flickering every time you moved the selection. It looked like a compositing
bug. It was actually a `memcpy` that had started copying the wrong thing and
could not stop. This entry is the short version of how a single triple-buffer
constraint became a three-frame ghost story.

{/* truncate */}

## The constraint: there is no third buffer

The 32X is already at 94% of its per-core RAM budget. The framebuffer pool is
two buffers — `framebuffer1` and `framebuffer2` — and the LCD driver
double-buffers between them. There is no room for a third. Other cores that
need a "clean snapshot of the game frame behind the menu" can keep one in spare
SRAM; the 32X cannot.

So the pause overlay (`md32x_repaint`) does the natural thing: on the first
menu frame, it remembers which buffer is **inactive** — that buffer still holds
the last pure game frame, because it was the one on screen the instant the menu
opened. It calls that the `frozen` buffer. Every subsequent menu frame, it
copies `frozen` into the active buffer, then darkens and draws the UI on top.
Two buffers, one of them frozen in time. It works on paper.

## The three frames where it falls apart

The menu loop runs the same sequence every frame: clear the active buffer,
repaint, darken, draw the dialog, swap. The bug is in how `frozen` and
`active` relate to each other across swaps. Walking it frame by frame:

**Frame 1** — `active = FB1`, `frozen = FB2`. The loop clears `FB1`. `repaint`
runs `memcpy(FB1, FB2)`, copying the pure game frame out of the frozen buffer.
UI drawn on top. This is the one frame that works.

**Frame 2** — `active = FB2`, `frozen = FB2`. `lcd_swap` has flipped the
pointers. The frozen buffer — the one that held the pure game frame — **is now
the active buffer**, and the loop's first step is `lcd_clear_active_buffer`,
which zeroes it. The only copy of the pure game frame is gone, destroyed by the
clear that was meant to prepare a fresh canvas. The `memcpy(FB2, FB2)` that
follows is a no-op. UI drawn on black.

**Frame 3** — `active = FB1`, `frozen = FB2`. `FB1` cleared. Now the
`memcpy(FB1, FB2)` copies the *contents of FB2 as it was at the end of frame
2* — which is **the black background plus the previous frame's UI**. So the
previous frame's dialog gets copied into the new frame, the new frame's dialog
is drawn on top of it, and the result is two overlapping dialogs, one frame
stale. Move the selection and the stale one lags behind. That is the flicker.

From frame 3 on, the loop is stable — stably copying the previous frame's UI
forward onto itself, forever. The game frame never comes back.

## The fix is one line: stop copying

You cannot add a third buffer — there is no RAM. You cannot stop clearing the
active buffer — the menu dialog needs a clean canvas. The only thing you can
stop doing is the copy, and only at the moment the copy becomes a lie.

```c
if (frozen == active) {
  /* lcd_swap made the frozen buffer the active one, and the menu
     loop's lcd_clear has just zeroed it. The pure game frame is
     gone; copying now only ghosts the previous frame's UI. */
  frozen = NULL;
} else {
  memcpy(active, frozen, 320 * 240 * sizeof(uint16_t));
}
```

Once `frozen` is `NULL`, the menu draws on a clean black background from frame
2 onward. The game frame is visible for exactly one frame — the frame the menu
actually opened on — and then the menu is a menu, not a compositing accident.
The flicker and the smearing are both gone. The cost is one `if`.

## What I actually learned

The first attempt at this bug was a "make the copy smarter" direction — try to
preserve the game frame longer, cache it somewhere, re-fetch it. All of those
hit the same wall: **there is nowhere to put it**. The 32X does not have a
hidden third buffer and it does not have spare RAM to make one. The fix was not
to preserve more. The fix was to **notice the exact moment preservation became
impossible and stop trying**.

A `memcpy` is not a neutral operation when its source is allowed to move. Here
the source was "the inactive framebuffer", and "inactive" is a role that
swaps every frame — so the source silently became the destination, and the
copy started cloning the canvas onto itself. The bug was not in the `memcpy`.
It was in the assumption that `frozen` stayed frozen.
