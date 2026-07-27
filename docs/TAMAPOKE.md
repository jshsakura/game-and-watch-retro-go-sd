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

`TAMAPOKE` defaults to `0` and is deliberately absent from `.github/workflows/package.yml`, so a
normal release build cannot pick this up by accident.

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

Then build the firmware with the core enabled, using the usual CI flag set plus `TAMAPOKE=1`:

```sh
make release DOCKER=1 TAMAPOKE=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
             DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 \
             CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

And stage the card:

```sh
TAMAPOKE_UPSTREAM=~/TamaPoke ./tools/tamapoke/stage_sd.sh ~/TamaPoke/tools/sdcard/mons /mnt/sd
```

That rescales the sprite packs, regenerates the gallery thumbnails, packs the names, and copies
everything into place. Then flash the firmware from the same build.

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
| D-pad | move the focus; walking off the edge of a paged screen is upstream's swipe |
| A | act on the focused widget |
| B | back / cancel — the one gesture with no touch equivalent, since there is no "outside the dialog" to tap |
| B held 3 s | the release dialog, as upstream's 3-second press on the pet |

Ten of the eleven screens work identically. The ball minigame is the exception: upstream has you
tap the falling ball's own coordinate, which a cursor cannot chase in time, so it is a paddle moved
left and right — reusing upstream's `ballVX`/`ballVY` physics and deriving the impulse from
`ballX - paddleX`.

## Iterating on the layout

Do not flash to look at a screen. `./tools/tamapoke_harness/run.sh` renders all eleven on the host
as PPMs, compiling the firmware's own sources (read out of the Makefile, not copied) against the
real `gw_lcd.h`, so the pixel path is the same code the device runs.

It fails on two things that are otherwise easy to ship: anything drawing outside the panel, caught
by guard bands either side of the framebuffer; and a screen whose focus highlight is invisible,
caught by rendering it at two focus positions and requiring the pixels to differ. With no touch
panel, a screen you cannot see the cursor on is a screen you cannot use.
