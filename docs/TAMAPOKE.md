# TamaPoke on the Game & Watch

A port of [socquique/TamaPoke](https://github.com/socquique/TamaPoke) — a virtual-pet game
written for a Waveshare ESP32-S3 board with a 466×466 round touch AMOLED — to the Game & Watch's
320×240 panel and buttons.

Upstream's code is MIT. This port is GPLv2, like the rest of the tree.

## What you can and cannot hand out

**This repository contains no game assets and no trademarked names.** That is deliberate, and it
is what makes the source shareable:

- Sprites come from [PMD SpriteCollab](https://github.com/PMDCollab/SpriteCollab) under
  **CC BY-NC 4.0**, and depict characters owned by Nintendo / Game Freak / The Pokémon Company.
- The 151 species names are trademarks. `DEX_TBL` ships with null name fields and fills them in
  at startup from `/mons/names.bin` on the card (`Core/Src/porting/tamapoke/tamapoke_dex.cpp`).
  Without that file the game is fully playable and every species reads as `#001`.

So:

| | |
|---|---|
| ✅ Share the **source and tools** | Everything here. Each person generates their own assets and builds their own firmware — which is also what upstream's own `CREDITS.md` asks people to do. |
| ⛔ Do **not** share a built firmware, a `TamaPoke.bin`, a `/mons` folder, or a `names.bin` | Uploading any of those anywhere — a release, a Drive link, a forum post — is distribution. It puts CC BY-NC material and Nintendo's IP under your name, and a GPLv2 binary obliges you to ship corresponding source you do not have the rights to. |

There is **no `TAMAPOKE` flag any more** — the core is compiled into every build (`Makefile`'s
`TAMAPOKE_CXX_SOURCES` is unconditional), so a release does carry `TamaPoke.bin`. That is fine and it
is the line that matters: the *binary* is our own GPLv2 code, and what must never be published is the
`/mons` assets and `names.bin`, which no build produces and no release contains. Two packaging gates
refuse a release that has one.

## The core cannot be dropped onto a stock firmware

The launcher dispatches homebrew by matching a name in resident code
(`Core/Src/retro-go/rg_emulators.c`), so there is no generic loader: copying `TamaPoke.bin` to an
existing card does nothing, because no branch calls into it. **The firmware and the core have to
come from the same build.** Flash both.

(The general fix — a loader that takes any `/roms/homebrew/*.bin` via an entry-point header — would
be worth having on its own, and would let any homebrew be added without a rebuild. It is not part
of this port.)

## Building it

You need a checkout of upstream for the assets. Nothing from it enters this tree.

```sh
git clone https://github.com/socquique/TamaPoke ~/TamaPoke
cd ~/TamaPoke && python3 -m pip install -r requirements.txt   # Pillow
python3 tools/pack_pmd.py                                     # downloads and packs the sprites
```

Then build the firmware with the usual CI flag set (the core needs no flag):

```sh
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
             DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 \
             CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

And stage the card:

```sh
TAMAPOKE_UPSTREAM=~/TamaPoke ./tools/tamapoke/stage_sd.sh ~/TamaPoke/tools/sdcard/mons /mnt/sd
```

That rescales the sprite packs, regenerates the gallery thumbnails, packs the names, verifies the
finished container **the way the firmware reads it**, and copies everything into place. Then flash the
firmware from the same build.

### Verifying a built container

```sh
python3 tools/tamapoke/verify_assets_dat.py /mnt/sd/roms/homebrew/tamapoke_assets.dat
```

`stage_sd.sh` runs this and refuses to stage a card that fails it. The parsers in it are written from
the *firmware's* side — `PmdMon::load()`, `SdThumbs::get()`, `tamapoke_assets.cpp` — not from the
packers', because a converter that only reads its own output the way it wrote it cannot catch the two
disagreeing. They did: the firmware read every Pokédex thumbnail as a fixed 24×24 block of raw bytes
when the record is `u8 w, u8 h, u8 palCount, u16 pal[], u8 px[w*h]`, so all 151 were drawn out of the
wrong offsets through the wrong palette, for every release. Both sides were self-consistent.

It also prints the action census, which is the answer to "why does this species not play an eating
animation": measured over the 302 packs, `IDLE / WALKL / WALKR / SLEEP / HURT / ATTACK / HOP` are in
all of them, while `POSE` is in 58, `EAT` 54, `NOD` 52, `SIT` 52 and `BREATH` 50 — SpriteCollab simply
has no Eat sprite for 82% of species. The repacker never drops an action; `pickAct()` in
`tamapoke_ui.cpp` falls back to one every pack carries.

### Why the rescale is not optional

Upstream cuts sprites for a 466×466 panel. Measured across all 151 species: median 125 KB per
pack, worst **495,503 B** (#095, #130), and three packs can be live at once — the pet, the
previous form during the evolution flash, and the gallery detail view. With the 169 KB thumbnail
table that is about **1.14 MB**, against **724 KB** of `RAM_EMU` for the entire core. Upstream
never had to care because every one of those allocations went to an 8 MB PSRAM part.

At half scale the worst pack is **123,983 B** and thumbnails are 54 KB. The firmware's sprite
slots are sized to exactly that, so a full-size pack will not load.

Measured on the linked build: **508,168 of 741,376 bytes (68.5 %)**, of which 61,028 is code and
rodata.

## Controls

There is no touch panel, so a focus cursor stands in for the finger. The game's own hit-testing is
untouched: `onTap()`, `onSwipe()` and every per-screen handler still run exactly as upstream wrote
them, and are handed synthesised coordinates
(`Core/Src/porting/tamapoke/tamapoke_input.cpp`).

| | |
|---|---|
| ←/→ | move the focus along the row. **Clamps at both ends** |
| ↑ | the status card |
| ↓ | the Pokédex |
| **TIME** | the clock / settings screen (toggles) |
| **GAME** | the ball minigame (toggles) |
| A | act on the focused widget |
| B | back / cancel — the one gesture with no touch equivalent, since there is no "outside the dialog" to tap |
| B held 3 s | the release dialog, as upstream's 3-second press on the pet |
| PAUSE | the launcher's own menu (volume, brightness, language, exit) |

The two labelled keys carry the two screens they name, and that is not decoration. Before, every
sub-screen was reached by walking the cursor off an edge: ↓ was the settings screen and the Pokédex
was *press → six times until you fall off the end of the button row*. That is upstream's horizontal
swipe, faithfully ported — and a swipe is a deliberate gesture on a panel, where an overrun is just
what happens when someone presses → once too often. So overshooting a button threw the player onto
another screen. TIME and GAME were dead keys the whole time.

Two screens are not menus and do not use the cursor at all
(`tamapoke_ui_input_mode()`): in the ball minigame ←/→ are a **held axis** driving the paddle, and on
the training sack **A is a hit**. Upstream has you tap the falling ball's own coordinate, which a
cursor cannot chase, so it is a paddle — reusing upstream's `ballVX`/`ballVY` physics and deriving
the impulse from `ballX - paddleX`. The paddle is stepped by the UI tick rather than by the input
poll, so its speed comes from the clock and not from how fast the main loop happens to spin.

## Saving

The pet is a clock, not a game with a save file: it has to persist by itself. It writes constantly
and the card must not be touched mid-play (that is how the FAT gets corrupted), so the store lives in
RAM and is flushed at safe points only — `tamapoke_prefs_commit()`.

The safe points are the launcher's own, and they are hooks the core has to *ask for*
(`odroid_system_emu_init`): `sram_save` fires on every sleep or standby entry and inside
`odroid_system_switch_app()` before the card is unmounted, and `shutdown` fires on power-off. With
both left NULL — as they were — the only commits were the pause menu's Save row and the idle
timeout, so quitting to the launcher, holding POWER, or the low-battery auto-off each lost
everything since the last one.

## Design

One radius scale, one accent, one focus treatment, one frosted surface — the tokens are at the top
of `tamapoke_ui.h` and `drawTile()` / `drawSurface()` in `tamapoke_ui.cpp` are the only two things
that draw a widget's chrome.

This is worth naming because the port did not start that way. Nine screens carried five different
ideas of "selected": the starter rows inverted and took a ring, the action buttons inverted and took
a *pulsing* ring, the feed cells went white with a ring, the card's name strip drew two hairlines,
and the gallery, keyboard and clock filled with orange. Radii ran 2, 4, 6, 8 and 10 with no rule.
Each was defensible on its own screen and together they read as five half-finished screens.

Two of those inconsistencies were not cosmetic. The bottom panel was an opaque slab of `#383844`
where upstream has translucent white, and the labels on it are drawn in `inkColor()` — dark in the
day theme, so the stat labels were dark-grey-on-dark-navy and unreadable on hardware. And a ring
drawn as N concentric `drawRoundRect()`s keeps one radius while growing, so each pass cuts a
different corner arc: every focus ring in the port had notched corners.

## Iterating on the layout

Do not flash to look at a screen. `./tools/tamapoke_harness/run.sh` renders all twelve on the host
as PPMs, compiling the firmware's own sources (read out of the Makefile, not copied) against the
real `gw_lcd.h`, so the pixel path is the same code the device runs.

It fails on two things that are otherwise easy to ship: anything drawing outside the panel, caught
by guard bands either side of the framebuffer; and a screen whose focus highlight is invisible,
caught by rendering it at two focus positions and requiring the pixels to differ. With no touch
panel, a screen you cannot see the cursor on is a screen you cannot use.
