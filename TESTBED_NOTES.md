## ⚠️ Personal experimental testbed build

This is an **unofficial, personal experimental** full-source build of retro-go-sd for
the Nintendo Game & Watch. It carries work-in-progress features and on-device fixes
that are **not** in the official project and may be unstable.

- **Official / stable build:** https://github.com/sylverb/game-and-watch-retro-go-sd
- This fork: https://github.com/jshsakura/game-and-watch-retro-go-sd
- **Testers welcome** — bug reports and screenshots are appreciated. Use at your own risk;
  back up your saves first.

> Install: flash `retro-go_update.bin`, then unpack `gw_update.tar` onto the SD card so the
> firmware and the homebrew overlays (`/roms/homebrew/*.bin`, `*.ro`) come from the **same**
> build. ROMs/WADs are never bundled — supply your own.

---

## What this testbed adds (vs. upstream)

### WonderSwan / WonderSwan Color (oswan)
- One Piece: Grand Battle (8 MB cart) now boots and runs at full speed — implemented the
  missing V30 `0x0F` instruction group plus `REPNC`/`REPC`.
- Fixed the savestate / resume HardFault on 8 MB carts: mirror cart banks across the
  address space, force `CS=0` during the `WriteIO(0xC0)` bank replay, correct `INT 1`
  stack handling, and save/restore the frame-timing phase in the savestate.
- Fixed the sound-DMA boot hang / frozen screen (port `0x52`, frame-skip latch).
- All the resume-debug instrumentation has been stripped back out.

### Neo Geo Pocket / Color (RACE)
- Sound now resumes correctly after loading a savestate (re-arm the Z80 / IRQ chain).
- Fixed FIT / letterbox scaling corruption and the flickering bottom band.
- Scaling follows the global setting and defaults to FIT on first run.

### Music player (MP3)
- Focused MP3 player homebrew.
- Fixed album-art JPEG colors (BGR565 → RGB565 R/B swap in the decoder).

### Video player (MJPEG / AVI)
- MJPEG-AVI video player homebrew (shares the music overlay).
- ~8× faster SD reads via HW-SPI block reads, fixing busy-scene judder; optional timing HUD.

### DOOM (work in progress)
- Bringing up DOOM (doomgeneric) by running most of its code/rodata **XIP from QSPI flash**
  so the zone fits in RAM (zone ~469 KB; all engine init passes).
- Keeps the window watchdog alive during slow cold-XIP bursts; boot trace to
  `/roms/homebrew/../doom_trace.txt` for diagnosis.
- Not guaranteed playable yet — actively being debugged on this testbed.

### Other
- Wolf3D homebrew overlay (id-engine symbols namespaced to avoid overlay collisions).
- Misc display/scaling consistency fixes across emulators.
