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

### DOOM (next-hack flash-resident engine — NEW, not yet verified on hardware)
- Replaced the earlier doomgeneric bring-up with the **next-hack flash-resident DOOM
  engine** (GBADoom-derived): a tiny ~111 KB static zone, build-time pre-composed
  textures, and texture columns read straight from XIP flash. This removes the zone
  fragmentation that crashed doomgeneric on level load and the per-column lump re-fetch
  that made it run far too slowly.
- Engine renderer/game code runs XIP from flash; only the small writable zone and
  framebuffers live in a 256 KB-aligned AXI-SRAM window. doomgeneric stays in-tree and can
  be re-selected at build time with `USE_NHDOOM=0`.
- **Requires a converted IWAD.** Run the in-tree `MCUDoomWadUtil` on your own `DOOM1.WAD`
  to produce `doom1.mcu.wad`, and place it at `/roms/homebrew/doom1.mcu.wad` — the raw
  `DOOM1.WAD` will **not** work. First launch caches it to flash (one-time, a little slow).
- **This engine swap is freshly integrated and unverified on real hardware.** If it
  misbehaves, please share the serial / SD log (not a screenshot).

### Atari Lynx (Handy)
- The Handy core now runs **XIP from QSPI flash** (only the small glue stays in the RAM
  overlay), killing the RAM→flash veneer corruption that destabilised execution.
- Two on-run crashes fixed earlier (BS93 big-endian header parse, Mikey render-line
  bounds), surfaced by a host AddressSanitizer harness that now gates CI.

### Other
- Wolf3D homebrew overlay (id-engine symbols namespaced to avoid overlay collisions).
- Misc display/scaling consistency fixes across emulators.
