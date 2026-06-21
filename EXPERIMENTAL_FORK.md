# Experimental Testbed Fork

> **This is a personal, experimental fork** of
> [sylverb/game-and-watch-retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd).
> It tracks upstream but carries extra, in-progress features that are **not** in
> the official build. Use at your own risk.

## What this fork adds

| Feature | State | Notes |
|---|---|---|
| **Neo Geo Pocket / Color** (RACE core) | ✅ Playable | Boot, input, sound, savestates (4 slots), runs full speed. Savestate sound-resume fixed (`ngpRunning` was unsnapshotted) |
| **WonderSwan / Color** (oswan core) | ✅ Playable | Boot, input, sound, savestates; Fit/Full scaling + speed-up fixed; heavy action games run slow (V30MZ interpreter limit) |
| **Music player** (MP3) | ✅ | Browser, deck, album-art covers |
| **Video player** (MJPEG/AVI) | ✅ Playable | 320×240 MJPEG-AVI from SD. SD block-read made it smooth (decode 27%→97.7%); encode via the companion app |
| **PICO-8** (external `pico8.ro`) | ✅ | p8ram moved to the AXI pool so it no longer OOMs the DTCM heap after a firmware update |
| **Battery monitor + auto power-off** | 🔜 evaluating | OFW-accurate battery %, shutdown at low % to protect RTC (from upstream PR #53) |

A post-mortem of the trickier bugs (dead-ends + actual root causes) is in
[issue #9](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/9).

## Performance notes (WonderSwan)

WonderSwan runs the V30MZ CPU, which is heavier than the Neo Geo Pocket's
TLCS-900. Light/medium games (RPG, puzzle, board) run near full speed; the
heaviest sprite-driven action games (e.g. Rockman EXE) run at roughly 40% speed.
This is the hardware ceiling for an interpreter on this MCU — no safe
optimization closes it without a dynamic recompiler.

Applied CPU optimizations (all minimal-diff, mergeable upstream):
- Inlined the hot memory-read path into the CPU core
- Direct-branch for internal-RAM (VRAM) writes
- Cached the code-segment base per instruction in the fetch path
- `-O3` on the core, render-skip on dropped frames

Deliberately **not** done (negligible gain and/or save-integrity risk):
- External-flash (XIP) caching — no measurable gain, cache-coherency risk
- SD overclock — instability, doesn't help runtime (ROMs run from flash, not SD)

## Build / install

CI builds `retro-go_update.bin` on push; tagged builds are attached to
Releases. Flash `retro-go_update.bin` with the normal G&W update procedure.

## Relationship to upstream

`main` mirrors `sylverb/main` and is synced periodically. Features live on
topic branches and are combined here. Upstream's `wonderswan` branch only drops
in the core submodule and has no working integration; this fork's WonderSwan is
a complete, playable implementation.
