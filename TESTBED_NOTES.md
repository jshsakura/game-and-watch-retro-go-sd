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

### PC Engine CD / Super CD-ROM² (pce)
- CD-ROM² games now boot and play. *Ai Chou Aniki* runs from the MASAYA logo through
  the intro art into Stage 1 shooter gameplay.
- Two protocol-level fixes found on the host trace harness and applied to the shared
  device core: the `$18C0` **Super System Card signature** (so the game stops aborting to
  a halt), and the **ADPCM-from-CD DMA** path (`$180B` bit 1), which previously never
  completed the bulk read and looped the boot.
- Savestates include the full 256 KB CD RAM.
- ADPCM / CDDA audio is decoded-and-discarded for now (no CD sound yet), and the 4-slot
  save/load polish is still in progress. **Host-harness verified; on-device boot still to
  be confirmed** — please share a serial / SD log if it misbehaves, not a screenshot.

### Magnavox Odyssey² / Videopac (O2EM)
- New SD-ROM system driven by the O2EM core.
- BIOS loads from `/bios/videopac`.
- Multi-game carts get a small game-select overlay: UP/DOWN pick a game (0–9), A starts;
  it defaults to game 1 after ~5 s if you don't choose.

### Atari Lynx (Handy)
- The Handy core now runs **XIP from QSPI flash** (only the small glue stays in the RAM
  overlay), killing the RAM→flash veneer corruption that destabilised execution.
- Two on-run crashes fixed earlier (BS93 big-endian header parse, Mikey render-line
  bounds), surfaced by a host AddressSanitizer harness that now gates CI.

### Other
- Misc display/scaling consistency fixes across emulators.

> **Next up (in bring-up, not in this build):** ZX Spectrum and Commodore 64, currently
> being brought up on a host harness. DOOM and Wolfenstein 3D have been dropped from the
> testbed — see the post-mortem issue for the why.
