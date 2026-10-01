# SNES current status — 2026-10-02

[Integrated release](RELEASE_STATUS_2026-10-02.md) · [Completed work, #50](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/50) · [Remaining verification, #49](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/49)

The generic SNES ROM core is included in this fork's `testbed-full-20261002-0140`
release, with ROM launch, savestates and resume. It is separate from the native
Zelda 3, Super Mario World and Super Metroid ports. Support and compatibility are
properties of this fork; upstream has its own supported-system policy.

## Current source and defaults

Release tag source: `6db21a6136dd0eff0ef9a9d37e3cc7494ed4f12e`.
SNES submodule: `e33537c06618e88786f518221787a1e033ba303b`.
The runtime integration is `c07ace94`; the subsequent commit changes CI checkout only.

| Setting | Release default | Meaning |
| --- | --- | --- |
| `SNES_THUMB2_CPU`, `SNES_THUMB2_SPC` | 1 / 1 | Thumb-2 65816 and SPC700 paths |
| `SNES_SPIN_BAKE`, `SNES_BAKE_BULK` | 1 / 1 | Recognized wait-loop spans, including complete-lap bulk folding |
| `SNES_SPC_IDLE_SKIP` | 1 | N-SPC timer waits |
| `SNES_BUS_IN_ITCM`, `SNES_WRAP_ITCM` | 1 / 1 | Bus and opcode-wrapper placement |
| `SNES_ROMPAGE_FOLD`, `SNES_DSP_FASTPATH` | 1 / 1 | Fetch-page cache support for ROM sizes and DSP-1 boards |
| `SNES_PPU_VIRGIN_Z`, `SNES_PPU_BLEND_LUT`, `SNES_PPU_OPAQUE_TILE` | 1 / 1 / 1 | Existing PPU improvements |
| `SNES_SKIP_SPRITE_EVAL_ON_SKIP` | 1 | Skip sprite evaluation on skipped frames |
| `SNES_STRETCH_FOLLOW` | 1 | Existing gap-free audio policy; production rate can affect pitch/speed |
| `SNES_AUDIO_RATE` | 16000 Hz when unspecified | Existing mono output rate |
| `SNES_LINE_CACHE`, `SNES_ROMCACHE`, `SNES_ROMPAGE_LOW` | 0 / 0 / 0 | Rejected or inactive paths; ROMCACHE=0 does not disable the C fetch-page cache |
| `SNES_SPIN_SKIP`, `SNES_DSP_MONO` | 0 / 0 | Runtime learner and alternate downmix disabled |
| `SNES_SMW_HLE`, `SNES_NSPC_HLE`, `RCSMW` | 0 / 0 / 0 | Experimental paths excluded from default behavior |
| `SNES_DEVICE_PROFILE`, `SNES_LOAD_DIAG`, `SNES_DEVICE_BENCH` | 0 / 0 / 0 | Release measurement hooks disabled |
| `SNES_STRETCH_PACKED_PICK` | 0 in source | Audio-search candidate disabled; hardware gain only +0.124% |

The existing default SNES clock is 312 MHz when the user has not selected a CPU
setting. CPU menu level 2 selects 340 MHz. This release does not automatically
select 340 MHz for every user.

Source of truth: [Makefile](../Makefile), [Makefile.common](../Makefile.common),
[main_snes.c](../Core/Src/porting/snes/main_snes.c), and
[snes_audio_stretch.c](../Core/Src/porting/snes/snes_audio_stretch.c).

## Hardware results and interpretation

At 340 MHz, the tested Super Mario World saved scene produced **60.132 drawn FPS**
with normal adaptive rendering, 900/900 frames drawn and measured dry-sample delta 0.
The relative improvement from that adaptive triplet is invalid because baseline
A drift was -0.819%, beyond the predeclared 0.5% limit. The candidate's absolute
frame/time observation remains recorded.

The identical-work 340 MHz triplet rendered every frame: A 57.915 / B 60.189 /
A 57.915, **+3.926%, drift 0%**. At 312 MHz, the equivalent comparison gave
54.322 → 58.586 FPS, +7.851%, drift +0.060%.

Zelda: A Link to the Past reached **57.245 drawn FPS** with every frame rendered
at 340 MHz. Bulk folding added only +0.041% in that scene. Zelda has not reached
60 FPS there. The earlier 51.03 / 52.65 results used different windows and are
preserved in the [2026-10-01 review](SNES_REVIEW_2026-10-01.md).

These are fixed saved-scene results, not whole-game minimum frame rates.
[Full conditions, invalid attempts, sound limits and evidence](SNES_DEVICE_RESULTS_2026-10-02.md).

## Validation and remaining work

Bulk folding passed 109,886 event/CPU-boundary differential spans with ASan/UBSan,
and 9 games / 12 harness conditions / 30,000 frames of state, video and full PCM
comparisons. All completed device triplets matched endpoint guest state, LCD and
raw DSP PCM. This does not prove byte-identical speaker output for a whole run.

SNES support/optimization work is merged and released. The final integrated
package's device installation, normal-mode audio and save/resume checks remain
in #49. No further optimization experiment is queued. A new Zelda 60 FPS effort
would require new CPU/PPU bottleneck evidence; this wait-loop change supplies no
meaningful remaining Zelda gain.

## Historical records

- [Read-only state review, 2026-10-01](SNES_REVIEW_2026-10-01.md): historical defaults, scene-specific Zelda measurements and native-port corrections.
- [Harness preparation, 2026-10-01](SNES_HARNESS_READY_2026-10-01.md): 26 completed runs / 30,000 guest frames; its OFF/pending language applies to that date.
- [Experiment history](SNES_NEXT_SESSION.md): earlier proposals, reversions and measurement corrections.
- [Compatibility survey](SNES_COMPATIBILITY.md): a historical host/rig survey, not complete device certification.
- [Optimization ledger](OPTIMIZATION_LEDGER.md): accepted and rejected paths with their premises.
