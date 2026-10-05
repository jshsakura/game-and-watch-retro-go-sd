---
id: release-status
title: 2026-10-02 release status
---

# Integrated release — 2026-10-02

[Download testbed-full-20261002-0140](https://github.com/jshsakura/game-and-watch-retro-go-sd/releases/tag/testbed-full-20261002-0140).
Install **`retro-go_update.bin`**; it carries the matching SD cores and support
files. **`gw_update.tar`** is for manual recovery.

The release includes [SNES ROM support](./super-nintendo.md), the latest
[32X optimizations](./sega-32x.md) and the [PicoDrive Sega CD port](./sega-cd.md).
SNES support refers to this fork's release, with game-specific compatibility
and performance limits.

## Measured results

| System / title | Device observation | Scope |
| --- | --- | --- |
| SNES / Super Mario World | **60.132 drawn FPS**, 340 MHz, 900/900 frames | Normal adaptive rendering in one saved scene |
| SNES / Zelda: A Link to the Past | **57.245 FPS**, 340 MHz | One saved scene with every frame rendered |
| 32X / After Burner Complete | **22.77 drawn FPS**, 340 MHz | Fixed boot/input benchmark window |
| 32X / Doom | **30.423 drawn FPS**, 340 MHz | Fixed boot/input benchmark window |
| Sega CD / Final Fight CD | About **60 drawn FPS** | Tested intro after hot-code placement |

These results are not whole-game minimums or final-package manual playthroughs.
SNES bulk folding is enabled by default; its default clock remains 312 MHz and
the existing audio policy is unchanged. Experimental measurement hooks and
the SNES audio-search candidate remain disabled.

## Verification

Local SD release and SD_CARD=0 builds, the full host suite, Lynx ASan/UBSan,
32X rig compile/link and SNES boundary/evidence tests passed. Package contents,
core tags and uploaded-asset hashes were verified.

The initial CI run passed all four test jobs, then failed package generation
because a nested PicoDrive dependency was missing. Recursive checkout is fixed
in `6db21a61`; the [replacement run](https://github.com/jshsakura/game-and-watch-retro-go-sd/actions/runs/36900868583)
was in progress at this documentation reconciliation. The run link records its
subsequent outcome.

Final integrated-package device installation, normal-mode sound/save/resume,
and large-ROM cache CRC/startup checks remain in
[#49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49).

Completed work is recorded in [#50 (SNES)](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/50)
and [#48 (32X)](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/48).
Upstream contribution decisions remain separate in
[#11](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/11).

[Detailed release identity and evidence](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/RELEASE_STATUS_2026-10-02.md) · [Issue index](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/ISSUE_STATUS.md)
