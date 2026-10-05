---
id: super-nintendo
title: Super Nintendo
---

# Super Nintendo

SNES ROM support is included in this fork's `testbed-full-20261002-0140` release,
using the generic LakeSnes core. Put `.sfc`, `.smc`, `.fig` or `.swc` files under
`/roms/snes/`. The port provides savestates and resume.

This is support in this fork. The native Zelda 3, Super Mario World and
[Super Metroid](./super-metroid.md) ports are separate programs with their own assets.

## Performance

At CPU menu level 2 (**340 MHz**), the tested Super Mario World saved scene reached
**60.132 drawn FPS** using normal adaptive rendering, drawing all 900 measured frames.
The measured dry-sample counter increase was zero. The existing default SNES clock
is **312 MHz** when no CPU setting has been selected.

Zelda: A Link to the Past reached **57.245 FPS** in its tested saved scene with
every frame rendered at 340 MHz. Bulk wait-loop folding, now enabled by default,
provided essentially no additional gain there.

These are scene measurements, not sustained whole-game minimum frame rates.
The SMW adaptive triplet's baseline drift exceeded the limit, so it supports the
candidate's absolute frame/time observation, not its relative improvement percentage.
The separate every-frame SMW comparison passed the drift check.

## Compatibility and sound

DSP-1 and Cx4 use clean-room HLE. Mega Man X2/X3 were checked on the device;
Pilotwings remains incompletely verified. Unsupported cartridge chips and
game-specific behavior can still prevent a title from working correctly.

The existing gap-free audio policy is unchanged. Slow production can affect
playback speed and pitch. The audio-search experiment and general N-SPC audio HLE
remain disabled by default. Reaching a frame-rate target in one window does not
establish sound quality for every scene.

Final integrated-package installation and normal-mode sound/save/resume checks are
tracked in [#49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49).

[Hardware conditions and evidence](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SNES_DEVICE_RESULTS_2026-10-02.md) · [Current defaults](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SNES_CURRENT_STATUS.md) · [Release status](./release-status.md)
