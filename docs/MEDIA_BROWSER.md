# Media Browser (homebrew app)

A homebrew app that turns the SD card `/media` folder into an on-device file
explorer, and (in later increments) opens the files it finds with built-in
viewers: text/EPUB reader, image viewer, and an MJPEG video player.

It is wired exactly like the existing Celeste / Zelda 3 / Super Mario World
homebrew apps: the code is built into an overlay section, exported to
`/roms/homebrew/Media.bin`, loaded from the **SD card** into `RAM_EMU` at
runtime, and dispatched by name from `rg_emulators.c`.

## Why this is feasible (and SNES/PDF are not)

A viewer is not real-time emulation. It only reads bytes from SD and draws
glyphs/pixels to the framebuffer, so it fits comfortably in the ~724 KB
`RAM_EMU` budget and the 280 MHz CPU. Every building block already exists in
the tree:

| Need                | Reused from                                              |
|---------------------|---------------------------------------------------------|
| Folder listing      | `rg_storage_scandir()` (`Core/Src/retro-go/rg_storage.c`)|
| Text + CJK glyphs   | `i18n_draw_text*()` / `odroid_overlay_draw_text()`      |
| DEFLATE (EPUB zip)  | zlib in `external/blueMSX-go/deps/zlib`, miniz          |
| XHTML parsing       | TinyXML in `external/blueMSX-go/Src/TinyXML`            |
| PNG decode          | `retro-go-stm32/components/lupng` (+ miniz)            |
| JPEG decode         | `Core/Src/porting/lib/hw_jpeg_decoder.*` (HW codec)     |
| Audio out           | SAI (`gw_audio.c`), 16-bit mono                          |

## Storage model

```
External flash (OSPI, the modded chip)     SD card
= firmware + big code/data                 = /roms, /cores, and:
                                             /media/...            <- user files
                                             /roms/homebrew/Media.bin  <- this app
```

`Media.bin` lives on the **SD card**; it is copied into `RAM_EMU` only while
running. It does **not** permanently consume external flash.

## Increment roadmap

1. **File explorer** (DONE — `main_media.c`): browse `/media`, D-pad to move,
   A to enter a folder, B to go up / exit at the root.
2. **Text viewer**: open `.txt` with paging via `i18n_draw_text()` (Korean OK).
3. **Image viewer**: `.png` via lupng, `.jpg` via the HW JPEG decoder.
4. **EPUB reader**: unzip (miniz) → OPF/XHTML (TinyXML) → reflow text.
5. **MJPEG video player**: `.avi` (MJPEG + PCM) demux → HW JPEG → SAI audio.
6. **Screenshots** (separate, later): a button combo in `common_emu_input_loop`
   that writes a PNG into `/media/screenshots/`, viewable in this same browser.

### Video format

Only **MJPEG inside `.avi`** can play (no H.264/HEVC decoder on this chip).
Convert on a PC first — this is exactly Tim's recipe:

```
ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 \
  -vf scale=w=320:h=240:force_original_aspect_ratio=decrease:force_divisible_by=16,format=yuv420p,scale=src_range=1:dst_range=1 \
  -r 30 -c:a pcm_s16le -ac 1 -ar 24000 output.avi
```

Every option maps to the hardware: `force_divisible_by=16` → JPEG MCU,
`yuv420p` → the HW decoder's native output, `pcm_s16le -ac 1` → the SAI's
16-bit mono path.

## Build wiring (mirrors Celeste)

| File | Change |
|------|--------|
| `Core/Src/porting/media/main_media.c`, `Core/Inc/porting/media/main_media.h` | the app |
| `STM32H7B0VBTx_SDCARD.ld` | `.overlay_media` / `.overlay_media_bss` (`AT > CORES`) |
| `STM32H7B0VBTx_FLASH.ld`  | `.overlay_media` / `.overlay_media_bss` (`AT> EXTFLASH`) |
| `Core/Inc/gw_linker.h` | `extern _OVERLAY_MEDIA_*` symbols |
| `Makefile` | `MEDIA_C_SOURCES`, `MEDIA_C_INCLUDES`, `-j .overlay_media` |
| `Makefile.common` | `MEDIA_OBJECTS`, include path, compile rule, `mkdir`, link rule, `Media.bin` export + sdpush |
| `Core/Src/retro-go/rg_emulators.c` | `#include "main_media.h"` + `"Media"` dispatch branch |

## Install / test

1. Put one or more files under `/media` on the SD card (start with a folder of
   `.txt`/`.png` — viewers land in increments 2-3).
2. Build, which produces and pushes `/roms/homebrew/Media.bin`.
3. On the device, open the **Homebrew** system and launch **Media**.

## Open questions / TODO

- **CJK in `odroid_overlay_draw_text`**: confirm it routes through the i18n
  font path; if not, switch the list renderer to `i18n_draw_text_line()` so
  Korean filenames render. ASCII is fine today.
- **RAM during playback**: the video player must run with emulators suspended
  (it owns `RAM_EMU`); budget the JPEG work buffer + SD ping-pong + audio.
- **Audio rate**: the recipe is 24 kHz; the SAI default is 48 kHz — the player
  resamples or reconfigures SAI.
- **Button mapping** for the future screenshot combo must avoid the existing
  menu/pause combos.
