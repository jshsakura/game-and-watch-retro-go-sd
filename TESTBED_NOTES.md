## ⚠️ Personal experimental testbed build

This is an **unofficial, personal experimental** full-source build of retro-go-sd for
the Nintendo Game & Watch. It carries work-in-progress features and on-device fixes
that are **not** in the official project and may be unstable. Everything here was
tested by one person on one device, so it will not be perfect.

- **Official / stable build:** https://github.com/sylverb/game-and-watch-retro-go-sd
- This fork: https://github.com/jshsakura/game-and-watch-retro-go-sd
- **Testers welcome** — bug reports and logs are appreciated. Use at your own risk;
  back up your saves first.

> Install: flash `retro-go_update.bin`. Flashing also pushes the matching support files
> (`/cores/*`, `/bios/logo.bin`, homebrew overlays) onto the SD card automatically, so
> no manual SD step is needed; `gw_update.tar` is provided for manual recovery only.
> ROMs are never bundled — supply your own.

---

## How this build differs from upstream

Upstream is the reference; this is simply a list of the differences, with honest
caveats where things fall short.

### Additional systems

- **PC Engine CD** — SCSI CD drive emulation on top of the existing pce-go port:
  CD-DA audio, ADPCM (MSM5205) streaming, BRAM saves on SD, savestate/resume including
  mid-ADPCM and CD-DA state. Per-game folder layout (`/roms/pcecd/<game>/` with
  cue+tracks), cover art per folder. Verified through four full playthroughs.
- **Atari Lynx** (handy) — runs with in-game save/load and resume; 512K carts execute
  bank0 in place from flash when RAM is tight.
- **WonderSwan / Color** (oswan) — 8 MB carts incl. One Piece: Grand Battle (missing V30
  instructions implemented), sound-DMA boot hang fixed, FIT-scaling artifacts fixed.
  Known limit: One Piece's savestate resume needs more cycle accuracy than oswan has.
- **Neo Geo Pocket / Color** (RACE) — runs from flash, flicker/scaling fixes, sound
  correctly resumes after loading a savestate.
- **ZX Spectrum** (floooh's chips core) — BIOS from SD, auto-fit screen, PAUSE-menu
  configurable GAME/TIME/B key mapping.
- **Commodore 64** (Frodo) — `.d64` loading with autostart (LOAD/RUN + warp), both
  joystick ports, pause/exit menu.
- **Odyssey² / Videopac** (O2EM, already in the upstream tree) — enabled, with the
  raw-ROM `getromdata` path fixed; save/load/resume; multi-game cart select overlay.
- **game.com** (Tiger) — plays the library; the 4-action pad mapped onto G&W buttons.
- **Virtual Boy** (red-viper) — honest caveat: **about 65-70% of full speed** with
  automatic overclock. Gapless audio, selectable pad presets (left pad / right pad /
  triggers as A/B). Several titles are enjoyable at that speed.
- **Tamagotchi P2** — runs on the same TamaLib core as the upstream P1.

### Apps (homebrew overlays)

- **Music player (MP3)** — minimp3 streaming, album art (HW JPEG + PNG, correct
  colours), ID3 tags, duration/seek; keeps playing across device sleep and while
  browsing the list.
- **Video player (MJPEG-AVI)** — work in progress: watchable, with faster SD reads and
  sleep recovery, but busy scenes can still judder (SD bandwidth bound).

### Launcher

- **Favorites tab (★)** — plain-text `/favorites.txt` (one ROM path per line) shown as
  the first tab; costs zero resident RAM (reuses the shared list buffer). Toggle from
  the A-button menu; mixed-system covers letterbox into one poster slot so the carousel
  stays aligned.
- **Wordmarks & icons** — per-system name headers in one font at one letter size,
  28×28 colour tab icons. NES is labelled `NES (FAMICOM)`, MSX `MSX / MSX2+`.
- **Carousel wrap** — lists that fit on one screen no longer repeat to fill the view;
  only lists longer than a page connect end-to-start.
- **i18n** — the added strings are translated across the 12 supported languages; older
  SD language bins remain compatible.

### System-wide

- **Game caching speed** — the ROM flash cache erases with the chip's largest erase
  command instead of per-4KB sectors; "Caching game" is several times shorter. Two
  latent edge cases fixed along the way (a buffer overflow on 256KB-sector chips, and
  a missed invalidation of the erased tail).
- **Blit speed** — the framebuffer MPU regions are Normal non-cacheable instead of
  Strongly-Ordered, saving ~1.4-2.1 ms per full-screen blit on every system, with
  explicit ordering guards (DSB before the vblank flip; per-frame clears wait out a
  pending flip).
- **Battery gauge** — filter state persists across power-off in an RTC backup
  register, with a time-based display limiter; the shown percent no longer seesaws
  between boots.
- **Idle auto-sleep** — an untouched game puts the device to sleep instead of draining
  the battery.
- **Sleep recovery** — SD file handles (music, video, PCE-CD streams) self-heal after
  the SD card is power-cycled by sleep.

The debugging history behind most of this is written up in
[`docs/UPSTREAM_ENGINEERING_NOTES.md`](docs/UPSTREAM_ENGINEERING_NOTES.md).
