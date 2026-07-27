# TamaPoke port — GLM work brief

Private, personal-use port of [TamaPoke](https://github.com/socquique/TamaPoke) (MIT) from a
Waveshare ESP32-S3 round AMOLED board to the Game & Watch. Target: a flashable build today.

**This build is never published.** The sprites are CC BY-NC and depict Nintendo characters; the
code is fine but the assets are not redistributable. Guarded by `TAMAPOKE=1` defaulting off and
by never appearing in `.github/workflows/package.yml`.

## Where to work

```
/home/ubuntu/app/jupyterLab/notebooks/gnw-tamapoke      worktree, branch feat/tamapoke
```

⛔ **Do not touch `/home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd`.** That is the
user's working folder, currently on `perf/32x-histogram`, and its branch must not change.

Upstream source to port from (read-only reference):

```
/tmp/claude-1001/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/\
fca786d8-3013-4df5-b9fc-73406eacbd65/scratchpad/TamaPoke/
```

## Your scope

Port `TamaPoke.ino` (2,351 lines) to `Core/Src/porting/tamapoke/tamapoke_ui.cpp`, **relaid out
from a 466x466 circle to a 320x240 rectangle**. That is the single biggest chunk of the port and
it is yours end to end.

Not yours (already written or in progress on the Claude side): the GFX shim implementation, the
input layer, the sprite loader, audio, RTC/save, and all build wiring.

### The API does not change — only the coordinates

`Core/Inc/porting/tamapoke/tamapoke_gfx.h` gives you a `Gfx` class whose method names and
signatures **match Arduino_GFX exactly**, and an `extern Gfx *gfx`. So `gfx->fillRoundRect(...)`,
`gfx->setTextSize(...)`, `gfx->print(...)` all keep working verbatim. Do not invent new drawing
calls; if you need one that is not in the header, say so and it gets added.

This is deliberate: it keeps your diff to layout, which is the part that actually needs judgement.

### Suggested first-pass transform

The source is a 466x466 circle; we have a 320x240 rectangle. A uniform 0.5x lands the whole
circle inside our height with room to spare, and the leftover width becomes side margin:

```
x320 = round(x466 * 0.5) + 43     /* (320 - 233) / 2 */
y320 = round(y466 * 0.5) + 3      /* (240 - 233) / 2 */
```

Apply that mechanically, then hand-tune per screen. The rectangle has corners the circle never
had, so the header/battery/streak badges can move outward and buy vertical room back.

★ **Text will not survive the same 0.5x.** Upstream uses `setTextSize(2..4)` on a 466 panel;
halving those gives 1..2, and size 1 is a 6x8 cell that is unreadable on this display. Re-pick
text sizes by eye per screen — size 2 (12x16) is the practical floor for body text. This is the
single most likely thing to come back looking wrong, so treat it as its own pass.

Sprites are handled for you: the packs are being regenerated at 0.5x, so the existing integer
zoom factors stay meaningful. Do not rescale sprites in the draw code.

### Focus highlights

Touch is replaced by a focus cursor (`Core/Src/porting/tamapoke/tamapoke_input.cpp`, already
written). The hit-testing functions — `onTap`, `galleryTap`, `keyboardTap`, `clockTap`, `gameTap`,
`onSwipe`, `onSwipeV` — are called with **synthesised coordinates** and must keep their bodies
unchanged. Preserve them as-is.

What you add is one line per widget so the focused one is visibly focused:

```c
for (int i = 0; i < 4; i++) {
    gfx->fillCircle(bx[i], by, BTN_R, faceColor(i));
    if (i == tamapoke_input_focus())
        gfx->drawCircle(bx[i], by, BTN_R + 3, INK);
}
```

Rings for the circular buttons, a 2px `drawRoundRect` plus inverted fill for rectangular buttons,
a cell border for the gallery grid, inverted key for the keyboard, underline for the food strip.
The pet zone should not get a rectangle — darken its ground shadow instead.

Export the layout constants your relaid-out screens use (`BTN0_X`, `EVO_BTN_*`, `GAL_*`, `KB_*`,
`PET_ZONE_*`, …) from `Core/Inc/porting/tamapoke/tamapoke_ui.h`; the input layer's hitbox tables
read them, so there is exactly one source of truth for each coordinate.

### Scope cuts for the first flashable build

Land these in order and stop wherever the day ends — each is independently useful:

1. Main screen: scene, pet sprite, 4 stat bars, 4 action buttons, poops, header, battery
2. Feed menu, bath, evolution FX, choice dialogs, release confirm
3. Stat card (4 pages)
4. Gallery, on-screen keyboard, clock screen — **defer these if time is short**

The ball minigame needs a real redesign (touch hits the ball's coordinate; a cursor cannot).
Convert it to a paddle: a bar at the bottom moved with left/right, reusing upstream's `ballVX`/
`ballVY` physics and computing the impulse from `ballX - paddleX`. Leave it for last.

## House rules

- No backticks in commit messages — the user's shell executes them.
- Never force-push a shared branch.
- Do not commit sprite data. `/mons/*.bin` and `tools/sdcard/` are gitignored; keep it that way.
- Build verdicts come from Docker: `make release DOCKER=1 <flags>`, never bare `make docker`
  (it discards command-line variables).
- Run `git submodule update --init --recursive` in the worktree before the first build.

## Done means

`tamapoke_ui.cpp` compiles against `tamapoke_gfx.h`, every screen you landed is laid out inside
320x240 with no primitive drawing out of bounds, and the focused widget is visibly distinct on
each one. Report which of the four scope tiers you finished.
