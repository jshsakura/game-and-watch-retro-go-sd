# SNES — where this stands, and the first command to run

Written 2026-08-10 with the device asleep. Everything below is measured unless it
says otherwise.

## State

| | |
|---|---|
| Shipped release | `testbed-full-20260810-0136` (CI green, `retro-go_update.bin` + `gw_update.tar`) |
| Real-gameplay baseline | **52.36 fps** — Zelda 3 rain, savestate resume, 900-frame window, ±0.09 |
| Branch | `testbed` @ `33013f9e`, submodule `external/sm` @ `0ab28db` (`perf/spc-idle-skip`), both pushed |
| Write-up | issue #39 · blog post `the-frame-counter-was-lying` |

## First command next session

The device sleeps on the launcher's idle timeout, so wake it, then:

```
GNW_AUTOBOOT=0 bash tools/gnw_probe/arm.sh build sprskip \
    GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=0 SNES_SPRITE_SKIP_DRAW=1
bash tools/gnw_probe/arm.sh flash sprskip
for r in 1 2 3; do bash tools/gnw_probe/bench.sh /tmp/gnw_arms/sprskip/gw_retro_go.elf 900; done
```

Compare against **52.36**. That is the one thing left already built and waiting:
`ppu_evaluateSprites` decodes and writes sprite pixels into `objBuffer` *before*
`ppu_runLine`'s frameskip return, so three frames in four write a buffer nobody
reads. Rig says hashes identical and +626 insn/frame (the test being paid with
nothing to skip, because the rig draws every frame). The device has not spoken.

Turn `SNES_SPRITE_SKIP_DRAW` on in `Makefile.common` if it wins; delete the block
if it does not.

## How to measure anything here

Two layers, and which one is allowed to judge what is not negotiable:

- **QEMU Cortex-M7 container decides correctness.** `run_snes_t2.sh` — four hashes
  (state, audio, both framebuffer windows). Any hash change means the work is
  wrong and stops there. It cannot see a cache miss, so it does not get a vote on
  speed. **But when its instruction count moves the wrong way, that is a warning
  that was overruled twice and cost 4.8% once.**
- **Raspberry Pi 5 + ST-LINK V2 decides speed.** `tools/gnw_probe/arm.sh` builds,
  flashes (including `cores/snes.bin` — the SNES core lives on the SD card, so
  flashing the internal image alone benchmarks the *previous* arm), and benches a
  900-frame deterministic window from a savestate.

**Always `cmp` the two arms' `snes.bin` before believing an A/B.** `FLAGS_STAMP`
did not record the SNES define groups until today, so toggling a knob rebuilt
nothing and both arms were the same binary.

## The rule

The core is **stall-bound, not instruction-bound**: −5.5% instructions bought
+1.9% speed. So a per-iteration test that skips work is paid on every iteration
that fails it, and in the scenes that are slow, most iterations fail.

**Ask: does it remove work, or add a test to skip work?** Expect the second to
lose. Every lever that won deleted something outright.

Caveat, learned at −0.64 fps: this is not "delete every check". Two DSP idle fast
paths skip a whole BRR decode and a Gaussian interpolation — real work — and
deleting those lost at once. The distinction is the *size of what is skipped*
against the cost of the test, not the shape alone.

## Measured and closed — do not re-propose

| | fps | |
|---|---|---|
| colour-math compositing, 2 px/iteration | 47.99 | −4.8% |
| DSP idle-voice BRR skip | 48.85 | −3.1% |
| sprite two-pass reverse draw | neutral | 8 loads removed vs 4 stores added per sliver; ratio is scene-independent, so a sprite-heavy scene changes nothing |
| whole-D-cache clean before present | 52.17 | −0.19, 3× the spread |
| pacing reference advanced by one period | 57.10 vs 57.40 | a 21 ms frame advances the tick counter by 1, same as a 14 ms frame |
| DSP idle-skip delete + channel pointer hoist | 51.72 | −0.64 |
| channel pointer hoist alone | 52.17 | −0.19; gcc had already folded it, rig delta was **+1 instruction** |

Also closed by reading the tree, not by measurement: inline WRAM fast path (tried
and dropped in `64bf5216`), compact bank map (`snes_cpuRead` already classifies in
three compares plus a page cache), layer masking and empty-tile skip (already in
`ppu.c`), DMA2D line output (already shipping).

## Candidates not yet measured

Both need a count before any code, because two levers were built on estimates
today and both lost.

1. **`PpuWindows_Calc` computed twice per line.** It takes no `sub` argument, so a
   layer windowed on *both* screens computes the identical result twice
   (`ppu.c:1207/1323/1436/1522`). Memoising per (layer, line) is a removal.
   **Count first:** how many layers per line are windowed on both screens in a
   scene that has a subscreen at all. If the answer is "almost none", it is worth
   nothing.
2. **`ClearBackdrop` on `bgBuffers[0]` and `[1]`** — 1 KB per line, 229 KB per
   drawn frame. Removing it needs a "is the subscreen all backdrop" test, which is
   the shape that keeps losing. Probably closed; worth a count only.

## Where the frame goes (52.36 fps build, real gameplay, 900 samples)

```
Thumb-2 engine     18.9%
app_main_snes      13.0%   dot scheduler
ppu_runLine        13.0%   sprite evaluation + compositing
dsp_cycle          10.8%  ┐
apu_run/cycle/spc  11.2%  ┘ APU 22.0%
snes_cpuRead        8.2%
PpuDrawBackground   7.1%
```

Three roughly equal thirds, no dominant item. The easy removals in the CPU and
scheduler lanes are spent. What is left is structural — collapsing loops in the
APU, or a different 65816 dispatch.

## Honest ceiling

60 fps in this scene needs the render at 7.4 ms instead of 17.65 (2.4×). Nothing
identified comes close. Lighter scenes already sit at 57–59, and the audio-DMA cap
is 60.15 regardless.

## Two stale claims in older docs

- `docs/SNES_PEAK_FRAME_ANALYSIS.md` closes shift-on-reuse partly on "the device
  renderBuffer is ONE LINE (`renderPitch = 0`)" and "the LCD framebuffer is
  outside the PPU module's scope". **Both are false for the shipping build** —
  `SNES_DIRECT_VIDEO` points `renderBuffer` at a persistent 158,720-byte
  full-frame buffer. Its other three reasons still stand, so the lever stays
  closed, but not for those reasons.
- `docs/SNES_LAST_MILE.md`'s original roadmap had four of five items already
  shipping or already measured as losses. Its addendum records which.

## Diagnostics that exist now

`SNES_PACE_OFF=1` (raw rate, no audio wait) · `SNES_FRAME_HIST=1` +
`tools/gnw_probe/frame_hist.py` (per-frame work histogram over SWD) ·
`SNES_PPU_SPLIT=1` (stop gcc folding three render functions into `ppu_runLine`) ·
`SNES_SPRITE_CENSUS=1` (slivers per line) · `SNES_READ_PROFILE=1` ·
`GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=n` (resume a savestate, same scene every arm).

`SNES_PACE_RING` is shelved and must not be enabled: an earlier form of it took
the device down with an imprecise bus fault that was never explained. Read its
comment in `main_snes.c` first.
