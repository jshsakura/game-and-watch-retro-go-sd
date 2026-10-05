# Issue status — reconciled 2026-10-02

[Release status](RELEASE_STATUS_2026-10-02.md) · [Live GitHub issues](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues)

This index distinguishes completed development records from remaining work.
It is a dated reconciliation; the linked GitHub issues retain subsequent updates.

## Remaining work

| Issue | Scope | What remains |
| --- | --- | --- |
| [#49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49) | Integrated release verification | Corrected CI package result, final device installation, normal-mode SNES/32X checks, and large-ROM cache CRC/startup behavior |
| [#11](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/11) | Upstream contribution tracker | Owner/maintainer decisions and any separately prepared submissions; a feature shipped in this fork is not automatically accepted upstream |

No implementation or device measurement is requested by this index. Completion of
the local gates is distinct from installing the final package on the device.

## Completed records

| Issue | Disposition | Evidence |
| --- | --- | --- |
| [#50](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/50) | SNES support and bulk-wait series complete, merged and released | [Device results](SNES_DEVICE_RESULTS_2026-10-02.md); SMW saved scene 60.132 FPS at 340 MHz, Zelda 57.245 FPS with every frame rendered |
| [#48](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/48) | Latest 32X optimization series complete, merged and released | [32X worklog](32X_WORKLOG_20261001.md); After Burner 22.77 FPS, Doom fixed window 30.423 FPS; final-package checks moved to #49 |
| [#12](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/12) | Harness/bring-up engineering notes retained as a completed historical record | [Current harness index](HARNESSES.md); historical bring-up limitations are dated in the issue |
| [#19](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/19) | Seven upstream-fix reference branches prepared; record retained | Reference preparation is complete; submission/adoption remains in #11, not an unperformed fork implementation task |
| [#28](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/28), #33–#35, #38–#42 | Earlier SNES experiments | Preserve the original windows, reversions and corrections; #50 is the latest release outcome |
| [#31](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/31) | Retired Sega CD implementation's RAM-limit record | Superseded for current support by the [PicoDrive port](SEGACD_WORKLOG.md); the original diagnosis remains historical |
| #45–#47 | Earlier 32X campaigns and rejected paths | Preserve their specific premises; the later #48 results supersede blanket claims that no further device gain exists |

## How remaining concerns are represented

| Concern | Status |
| --- | --- |
| Zelda sustained 60 FPS | Not achieved in the latest measured scene; future research would require a new task and fresh bottleneck evidence |
| 32X full speed / whole-library compatibility | Not established; measured title/window results remain below full speed |
| SNES audio-search candidate | Disabled: hardware benefit only +0.124%; no adoption task pending |
| 32X device audio endpoint hash | Invalid as audio evidence because captured after stop; rig full-stream hashes are retained |
| Old intermittent 32X freezes or profiler faults | Historical observations in #46/#47 and the ledgers; their presence or resolution in the final package has not been re-established |
| Large-ROM cached launch after CRC checking | Native tests passed; device launch cost and cache behavior remain in #49 |

Closed research paths are records, not guarantees about future architectures. Reopening
one requires naming a changed premise and recording a new comparison. Completion claims
must keep hardware observations, harness correctness and final-package installation separate.
