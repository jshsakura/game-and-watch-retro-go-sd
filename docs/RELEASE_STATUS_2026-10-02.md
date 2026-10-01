# Integrated release status — 2026-10-02

[Published release](https://github.com/jshsakura/game-and-watch-retro-go-sd/releases/tag/testbed-full-20261002-0140) · [Remaining verification, #49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49) · [Issue index](ISSUE_STATUS.md)

SNES support, the latest 32X optimization series and the existing PicoDrive Sega CD
port are integrated in `testbed-full-20261002-0140`. SNES support here refers to
this fork's release. Each system retains its documented compatibility and speed limits.

## Source and package identity

| Item | Identity |
| --- | --- |
| Release tag | `testbed-full-20261002-0140` |
| Tag source after CI checkout correction | `6db21a6136dd0eff0ef9a9d37e3cc7494ed4f12e` |
| Locally built runtime integration | `c07ace9441ac4571459b09b611a82b4b631b6eec` |
| SNES / Super Metroid submodule | `e33537c06618e88786f518221787a1e033ba303b` |
| PicoDrive submodule | `5cc10e1af34383c6c6a6f24420672f9055d0b30b` |
| Builder used locally | `sylverb/retro-go-sd-builder:v1.5`, Arm GNU 15.2.1 |

`6db21a61` changes recursive submodule checkout in CI only; runtime source and
submodule pins are identical to `c07ace94`. Both build identities use the same
firmware/core tag. The release tag was updated to include this CI correction.

The local integrated build passed link checks for 33 overlays. All 26 CORI-tagged
core files matched `Retro-Go SD testbed-full-20261002-0140`; the archive also
contained the matching 32X, Sega CD, GBA and Super Metroid XIP payloads. Every
archive file matched staging, the firmware was present, and no personal
configuration or saves were packaged. The internal firmware was 261,840 bytes,
leaving 304 bytes in its 256 KiB bank for this configuration.

Install `retro-go_update.bin`. It includes the updater and matching SD support
archive. `gw_update.tar` is the manual-recovery archive. Both uploaded assets
matched local SHA-256 hashes at publication. CI may replace them with its clean
build, so the current release asset metadata is the authority for downloaded bytes.

## SNES: adopted changes and measured limits

`SNES_BAKE_BULK=1` is the release default. The CPU menu and audio policy are
unchanged: the existing unselected SNES clock is 312 MHz; level 2 selects 340 MHz.
The audio-search experiment stays disabled (`SNES_STRETCH_PACKED_PICK=0`).
Completion-benchmark hooks and the device profiler are disabled in the release.

| Saved scene and mode | MHz | Candidate drawn FPS | Interpretation |
| --- | ---: | ---: | --- |
| Super Mario World, normal adaptive rendering | 340 | **60.132** | 900/900 frames drawn, measured dry-sample delta 0; A drift invalidates the relative-gain claim |
| Super Mario World, every frame rendered | 340 | **60.189** | A 57.915 / B 60.189 / A 57.915; +3.926%, drift 0% |
| Super Mario World, every frame rendered | 312 | **58.586** | A mean 54.322; +7.851%, drift +0.060% |
| Zelda: A Link to the Past, every frame rendered | 340 | **57.245** | +0.041%, drift +0.083%; essentially no additional gain from bulk folding |

These are fixed saved-scene windows, not sustained whole-game 60 FPS results.
The earlier Zelda 51.03 / 52.65 FPS observations describe different windows and
must not be chained into an improvement calculation. Native Zelda/SMW ports are
separate programs from the generic SNES ROM core.

Correctness evidence includes 109,886 differential spans with ASan/UBSan and
9 games / 12 harness conditions / 30,000 frames of state, video and full PCM
comparisons. Completed device triplets matched endpoint state/LCD/raw DSP PCM.
Endpoint PCM does not establish identical speaker output throughout a run.
Invalid drift comparisons and failed attempts remain in the evidence.

[Full hardware results](SNES_DEVICE_RESULTS_2026-10-02.md) · [Current SNES defaults](SNES_CURRENT_STATUS.md) · [Completed work, #50](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/50)

## 32X and Sega CD

The 32X series is merged and published: SH-2 native paths, PWM interrupt HLE,
68K decode/cache work, hot-code placement in ITCM/RAM_EMU, DTCM data placement,
64 KiB XIP alignment and flash-content CRC verification.

| 32X title | Latest recorded device window | Drawn FPS |
| --- | --- | ---: |
| After Burner Complete | Fixed cold boot/input; warmup 540, window 180; 340 MHz | **22.77** |
| Doom | Fixed boot/input; warmup 900, window 600; 340 MHz | **30.423** |

Those measurements belong to the optimization arms, not a new final-package
manual playthrough. They do not predict every title. The recorded 32X device
audio hash was captured after audio stopped and is all zeros; audio correctness
evidence for that series is the rig's full-stream comparison.

[32X worklog](32X_WORKLOG_20261001.md) · [Completed series, #48](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/48)

Sega CD is included through PicoDrive. The Final Fight CD intro reached about
60 drawn FPS after hot-code placement; save/load, resume, menu colors and Full
scaling were checked on the device. This is not a library-wide 60 FPS result.
The old gwenesis-based RAM-limit conclusion applies to the retired implementation.
Use original disc data with the matching regional BIOS; the old sector-0 patching
diagnosis was retracted. [Sega CD worklog](SEGACD_WORKLOG.md).

## Validation and remaining work

| Check | Recorded result |
| --- | --- |
| Full local host suite | PASS |
| Lynx ASan/UBSan | PASS |
| Canonical SD release and SD_CARD=0 builds | PASS |
| 32X rig compile/link with the pinned builder | PASS |
| SNES boundary, evidence and device-record tests | PASS |
| Link integrity, complete/fresh SD staging, package layout and core tags | PASS |
| Completion-benchmark symbols absent from release ELF | PASS |
| Published-asset hashes at upload | PASS |
| Initial CI host, Lynx, SD_CARD=0 and rig jobs | PASS |
| Initial CI release-package job | FAIL: missing nested `emu2413` headers |

The checkout defect is corrected in `6db21a61`. Its replacement
[CI run](https://github.com/jshsakura/game-and-watch-retro-go-sd/actions/runs/36900868583)
was in progress at this reconciliation; record its completion in #49 rather than
treating the earlier failed run as success.

Still unverified: final integrated-package device installation, normal-UI
sound/save/resume checks, and startup cost of CRC-checking a large cached ROM.
The separate SNES and 32X device measurements do not replace those checks.
No further optimization campaign is queued. #49 is the remaining release checklist;
future upstream contribution decisions stay in #11.

Local provenance is retained in the final-release worktree's `build/release-validation.json`,
`build/release-gates-r2/`, completed build logs and `build/published-release.json`.
ROM-free device evidence is committed under `tools/snes_device_bench/results/20261002/`.
