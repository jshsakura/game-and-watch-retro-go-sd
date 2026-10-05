---
id: sega-32x
title: Sega 32X
---

# Sega 32X

The PicoDrive 32X core is included in this fork's release. Put 32X cartridges
under `/roms/32x/`. Compatibility and speed vary by title; the measured titles
remain below full speed.

## Latest device measurements

| Title | Fixed device window, 340 MHz | Drawn FPS |
| --- | --- | ---: |
| After Burner Complete | Cold boot/input, warmup 540, window 180 | **22.77** |
| Doom | Boot/input, warmup 900, window 600 | **30.423** |

The 2026-10-01 work improved After Burner from 10.11 FPS through SH-2 native
paths, PWM interrupt HLE, 68K decode/cache work and hot-code placement in fast memory.
Those optimization arms are integrated in the 2026-10-02 release.

The values describe their recorded windows. They are not a manual playthrough
of the final package, and they do not predict the rest of the library. Earlier
Doom attract/gameplay figures apply to different builds and scenes.

The device screen hashes matched. The device audio endpoint was captured after
playback stopped and contained zeros; audio correctness evidence for the series
comes from the rig's full-stream comparison.

## Display options

`PAUSE → Options` provides **Full** scaling and **Screen tearing fix**, both off
by default. The tearing option trades speed for a cleaner picture; single-buffer
scanout can still tear at scene transitions.

Final normal-UI installation, sound and save/resume checks remain in
[#49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49).

[Latest worklog](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/32X_WORKLOG_20261001.md) · [Completed series, #48](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/48) · [Release status](./release-status.md)
