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

## The next target, with its ceiling already priced

**Rewrite the background tile-decode inner loop.** Ablating the background draw
entirely -- `SNES_ABLATE_BG=1`, wrong output on purpose -- reads **59.54 fps
against the 52.36 baseline: +7.18, +13.7%.** That is 3.5x everything won on this
core in a full day of A/Bs, and it is the only number on the board big enough to
justify a hand-written-assembly project.

A rewrite captures less than the ablation: it deletes the tilemap walk and VRAM
fetch too, which SIMD does not touch. A third to a half is the honest
expectation, so **+2.4 to +3.6 fps**.

Two things make it tractable, both measured today:
- The main screen already skips 46% of its tiles for free (transparent); the
  subscreen skips none. 68 decoded tiles per line across both screens.
- There is nothing to share between the passes -- 0 of 42,191 sub passes draw a
  layer the main screen also draws -- so this is one loop to make faster, not a
  duplication to collapse.

**And a warning about the instrument.** PC sampling scored
`PpuDrawBackground_4bpp` at 6.1% of the frame; ablation says 13.7%. A sampler
credits a stall to whichever instruction is retiring, so memory-bound work reads
lighter than it is, and this core is stall-bound. **Price a candidate by ablation
before designing for it.** That is now the first step for any large lever, and it
costs one build.

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

## Counted and closed — three more, none of them built

Counting cost three diagnostic builds. Implementing them would have cost days.

1. **`PpuWindows_Calc` computed twice per line** — 28,690 lines, 30,319 Calc calls
   (1.06 per line), of which the **subscreen pass accounts for zero**. No layer is
   ever windowed on the sub screen here, so there is no duplicate, and at 1.06
   calls per line it was not a hotspot either.
2. **Single-pass main+sub background render** — of 42,191 sub passes, **zero** draw
   a layer the main screen also draws. hScroll/vScroll belong to the layer, so a
   shared layer would decode identical tiles in both passes; it never happens. The
   subscreen is separate work, not duplication.
3. **Frameskip sprite-draw skip** — built and measured: 52.29 vs 52.36. Nothing.
   Sprite pixels are 5.46 slivers per line against a limit of 34.

## The render's shape, counted

Zelda 3 rain, 339,259 rendered lines:

```
                    main      sub
  layer passes/line  2.29     1.00      sub runs on 100% of lines
  tiles/line        65.1     33.0
  blank tiles        46%       0%       main skips nearly half for free
  decoded/line      35.2     33.0       <- sub is 48% of all tile decode
```

The subscreen exists only because colour math is on, draws one fully opaque layer
that shares nothing with the main screen, and skips nothing. What is left in the
render is not a duplication to remove but an inner loop to make cheaper: **68
decoded tiles per line across both screens, at roughly 17 cycles per
layer-pixel.** That is a hand-written-assembly project of the same shape as the
Thumb-2 65816 engine, not a knob.

## rc (static recompilation) — why it is not the next lever

It looks like the big one on paper: `docs/RESUME_GNW.md` records SMW at 8.20M →
4.63M instructions/frame, 1.77×, bit-identical. Three things kill it as a *next*
step, and the third is decisive:

- **That 1.77× was measured against the C interpreter**, before the hand-written
  Thumb-2 65816 engine existed. The engine has since taken a large part of the
  same ground. The gain against today's core is unmeasured and certainly smaller.
- **It is per-ROM.** Only SMW has a blob. The baseline scene is Zelda 3, which
  would get nothing.
- **`RCSMW=1` does not link today: "SNES interpreter ITC overflow".** The 270 hot
  sites are 23,504 B and the Thumb-2 engine already ends at 0x13FB8 — together
  they overflow the 65,280 B of ITCM by 16,568 B. rc and the engine want the same
  64 KB, and the engine is 18.9% of the frame with measured evidence that leaving
  ITCM costs it (a single veneer was 2.7%).

Also note `RCSMW ?= 0` and CI does not pass it, so **no shipped build has ever had
rc active** — `.itcm_rc_hot` is 0 bytes in the release map. The dispatch machinery
ships; the payload does not.

Reopening rc means one of: run the sites from XIP (the DOOM-vs-SM scale risk),
cut to ~40 sites (too few for the gain), or move the engine out of ITCM and pay
for it (never measured). Only the third is worth an experiment, and it is two
builds — but it is SMW-only either way.

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
