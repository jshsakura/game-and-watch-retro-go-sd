---
id: super-metroid
title: Super Metroid
---

# Super Metroid

A port of [snesrev/sm](https://github.com/snesrev/sm), the C reimplementation of Super Metroid —
not an emulator. It runs at the full 60 fps, with savestates and the launcher's own pause menu.

Unlike Zelda 3 and Super Mario World, it is **not** built from an assets file: the port reads the
original ROM at runtime. What the SD card needs is the ROM itself.

`/roms/homebrew/` must end up with three files:

| File | What it is | Where it comes from |
| --- | --- | --- |
| `Super Metroid.bin` | the core overlay — this is the entry you launch | the update, automatically |
| `sm.xip` | the game's cold code and rodata, cached into external flash on first launch | the update, automatically |
| `sm.smc` | the 3 MB ROM | **you supply it** |

The first two arrive on their own: `retro-go_update.bin` is a container — the updater app with the
whole SD payload (`gw_update.tar`) concatenated onto it — so the normal
[update steps](https://github.com/sylverb/game-and-watch-retro-go-sd#retro-go-sd-update-steps)
write them into `/roms/homebrew/` for you. They and the firmware are cut from one ELF and belong to
the same release; that is why they travel together, and why you should not hand-copy an older
`sm.xip` over a newer firmware.

So the only file you have to put there yourself is `sm.smc`.

To prepare the ROM:

```bash
python3 tools/sm_prepare_rom.py /path/to/your/super_metroid.smc -o sm.smc
```

Then copy `sm.smc` into `/roms/homebrew/`. The script exists because the port indexes the ROM as a
flat LoROM, so a dump carrying the usual **512-byte copier header** puts every read 512 bytes off
and the game dies without saying why. The script strips the header if there is one, checks the
size, and tells you whether the image is the stock ROM
(sha1 `da957f0d63d14cb441d215462904c4fa8519c613`) or something else.

A fan-patched ROM is a supported input, not an error: the port takes its **second language from
the ROM you supply**. With a stock image that second language is Japanese; with the Korean fan
translation applied it is Korean, and the port defaults to it. Switch back to English in the
emulator's Options menu at any time. No translation data is distributed here.

## Controls

Due to the limited set of buttons (especially on the Mario console), the controls are peculiar.
Super Metroid needs more buttons than Zelda 3 or Super Mario World did — it actually uses `L` and
`R` (aim down / aim up) — so on a Mario unit two of them live on `GAME` + a direction. While `GAME`
is held on a Mario unit, `UP`/`DOWN` act as those buttons and are not passed to the game as
directions; `LEFT`/`RIGHT` still are, so you can still aim diagonally.

| Description | Binding on Mario units | Binding on Zelda units |
| --- | --- | --- |
| `A` button (Jump) | `A` | `A` |
| `B` button (Dash) | `B` | `B` |
| `X` button (Shoot) | `TIME` | `TIME` |
| `Y` button (Item cancel — fire the selected item) | `GAME + UP` | `SELECT` |
| `Select` button (Cycle item) | `GAME + TIME` | `GAME + TIME` |
| `Start` button (Map / pause screen) | `GAME + DOWN` | `START` |
| `L` button (Aim down) | `GAME + B` | `GAME + B` |
| `R` button (Aim up) | `GAME + A` | `GAME + A` |

Super Metroid is SD-card only. A flash-only (`SD_CARD=0`) build cannot cache and relocate `sm.xip`,
nor hold the 3 MB ROM the port reads at runtime, so the core is left out of that image rather than
shipped as a dead menu entry.
