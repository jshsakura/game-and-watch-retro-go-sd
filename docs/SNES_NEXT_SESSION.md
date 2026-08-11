# SNES — where this stands, and what to aim at next

Rewritten 2026-08-11 and updated twice the same day: once after the layer loop
was taken apart, once after the whole frame was priced and the audio defect
diagnosed. Everything is measured on hardware unless it says otherwise: Zelda 3
rain, resumed from a savestate so every arm starts in the same scene, 900
deterministic frames for speed, 1800 for the audio counters.

**One decision is open and needs a human ear** — see "The audible defect" below.

## State

| | |
|---|---|
| Real-gameplay speed | **56.93 fps** emulated (from 55.51) |
| **Real-gameplay smoothness** | **16.61 drawn fps** (from 14.34) — **+15.8%**, and this is what the player sees |
| On by default | `SNES_PPU_VIRGIN_Z`, `SNES_PPU_BLEND_LUT`, `SNES_SKIP_SPRITE_EVAL_ON_SKIP` |
| Shipped release | `testbed-full-20260811-1944` — carries virgin-z, blend-LUT and the sprite skip; **predates all audio work** |
| Branch | `testbed`, submodule `external/sm` on `perf/spc-idle-skip`, both pushed |
| Device | idle power-off is compiled out of every arm now (`GNW_NO_IDLE_OFF=1` in `arm.sh`) |

## Read this first: two instruments, and neither is fps alone

**1. The overload guard draws one frame in four.** `bench.sh` counts *emulated*
frames, so the player sees a quarter of them — measured: 57.92 emulated against
14.85 drawn, ratio 0.256. **A change that makes skipped frames cheaper LOWERS
the fps number**, because the guard spends the slack on drawing more.
`SNES_SPRITE_SKIP_DRAW` was shelved a session ago as "nothing, 52.29 vs 52.36"
for exactly this reason. `g_snes_drawn_frames` read over SWD is the metric that
can see it.

**2. Audio counters need a frame-aligned window.** They accumulate from boot,
and a wall-clock window is not the same window twice: the same build read 0 and
then 181 underruns on two thirty-second samples — wider than most differences
being judged, and one of those readings was reported as a result before the
instrument existed. `tools/gnw_probe/stretch_ab.sh` takes the delta across a
fixed number of emulated frames; the scene is deterministic from the savestate,
so every arm sees the same audio.

Average frame = `14.6 + 17.65·d` ms where d is the draw fraction. Real time
(16.625 ms) needs d ≤ 0.11. Cheaper rendering raises **both** d and fps.

## The whole frame is now priced

| block | worth | how it was measured |
|---|---|---|
| remaining render (all of it) | **3.15 fps** | return before it; today already took 2.1 of it |
| DSP voices | **2.37 fps** | voices do nothing, `dsp_cycle` still emits |
| sprite path | 0.33 fps | emission 0, objBuffer wipe 0, merge +0.33, scan visits 0.93/line |
| 65816 interpreter | **no lever** | 9,080,349 opcodes, **0** C fallbacks |
| APU as a whole | unmeasurable | `SNES_ABLATE_APU=1` hangs the SPC boot handshake |

Inside the DSP's 2.37: echo 2.65% of all instructions, BRR decode 1.35%,
Gaussian interpolation 0.41%, ADSR envelope 0.06% — and over half is the
per-voice skeleton, 68% of whose ticks are idle voices. Both ways of deleting
those (hoist the idle test out of the call; drop idle voices from the loop with
a closed-form pitch fold) are bit-identical and measure **zero**: 56.75 and
57.01 against 57.00. **68% is a count, not a cost.**

So there is no single lever left anywhere. Everything measured is 0.3–0.6 fps or
zero.

## The audible defect, and the decision it needs

The rain crackles, and it is not a speed problem. At 57 fps the core makes
15,200 samples/s against 16,000 consumed — a 5% deficit that closes only at
60.0 fps, which the 3.15 fps of remaining render cannot reach.

The stretcher covered that deficit by repeating one waveform period, which is
seamless when the signal *has* a period. Rain is broadband noise, and the period
picker scored lags by raw correlation with **no confidence measure**, so it
returned whichever lag scored highest and spliced a random segment. That splice
is the crackle.

Fixed so far, all measured on 1800 aligned frames:

| | splices | underruns | pitch |
|---|---|---|---|
| always PICOLA (before) | 198 | 723 | kept |
| noise-aware band (**current default**) | 189 | 573 | kept |
| floor at 3% | 179 | 416 | −3% |
| `SNES_STRETCH_FOLLOW=1` | **0** | **162** | −5% |

Plus two fixes that help every mode: a dry ring now **fades** instead of holding
`last` (a DC step at both ends of a gap is itself a click), and the level term
answers a low backlog immediately instead of low-passing it in both directions.

**There is no useful middle.** The 3% floor was built to find one: three
quarters of the transposition buys ten splices out of 189. The deficit is either
covered by the rate or it is not.

**The open decision is which artefact to ship**, and it is the one thing no
counter can judge: a constant 5% transposition, or 189 splices and 573
dropouts per 1800 frames. `SNES_STRETCH_FOLLOW` is the switch.

## Closed, with numbers, do not re-derive

- **Reversed noise fillers.** Same spectrum, no new periodicity, seam continuous
  by construction — and it works, cutting periodic splices 213 → 89. But it
  fires more often and the ring runs dry doing it (219 underruns). Off.
- **Loosening the insertion guard** from `fill > pitch_est + 2` to `XFADE`. The
  argument is clean — the copy reads history, not the queue — and it takes the
  same window from 0 to 444 underruns. The guard is also keeping the level loop
  out of a state it cannot recover from. Off, with the reasoning written beside
  the line.
- **Smoothing noise-ness per passage** instead of per pull: 236 splices and 739
  underruns. Holding the band wide hands the level term more authority and the
  loop oscillates. The ±1% band is a stability limit, not only an audibility one.
- **`GNW_NO_BOOT_RESCUE=1`** is now in `arm.sh` for every measurement arm: an
  ablation arm that hangs is by definition a failed boot, and after two of them
  the rescue screen stops every later flash for 60 s waiting on a button nobody
  is at the console to press, then powers the unit off. Three rounds were lost
  to that loop.

## The next target

**A hand-written Thumb-2 tile loop — and this time it is the reachable part.**

The layer draw costs **4.4 fps** and the whole of it is the **pixel work**: one
`PpuDecode4bpp` plus eight × (nibble extract, z-compare, conditional store).
Not the fetch, not the walk, not the setup. All measured on hardware in one
build, Zelda 3 rain from a savestate, 900 frames, three runs per arm:

```
baseline                                     55.57   (55.38 on the recheck)
SNES_ABLATE_BG=6  pixel work gone,
                  fetch KEPT ALIVE           59.96   +4.4    <-- the answer
SNES_ABLATE_BG=4  return before the loop     59.73   +4.16
SNES_ABLATE_WALK=1                           55.46   nothing
tile memo / PLD / framebuffer / VRAM-in-DTCM 55.4-55.6  nothing
```

The rig agrees to the instruction: of the layer draw's **685,758** instructions a
frame, the pixel work is **663,322** — 97% of the instructions and 100% of the
time. There is no stall mystery here. It is ordinary work in ordinary quantity,
and that is why every attack on memory measured zero.

### Three things this retracts

**1. `SNES_ABLATE_BG=2` never measured what it claims.** It empties the pixel
macros, which leaves `bits` dead in the middle loop, so the compiler deletes
`READ_BITS` and both VRAM loads with it — =2 silently becomes =1. Its binary is
3,116 bytes smaller than the baseline, not the few hundred a pixel-only deletion
costs. `=6` is the honest version: it keeps the fetch alive with one volatile
store per tile. **Check what an ablation compiles to, not what its name says.**

**2. Worse, last session's =2 arm never got the define at all.** `Makefile.common`
only wired `ifeq ($(SNES_ABLATE_BG),1)`, so `SNES_ABLATE_BG=2` built the
baseline and reported "nothing". That false zero is the origin of the whole
memory theory: it said the pixel work was free, so the cost had to be the reads.
Four experiments and most of two sessions went to that. The knob passes any
value now.

**3. `SNES_ABLATE_FETCH` (+3.04) and `SNES_ABLATE_ADDR` (+2.09) are contaminated**
and must not be read as fetch prices. Both change `bits`, which changes how many
tiles are blank and how many pixels are non-zero — they move the pixel work they
were meant to hold still.

### Two attempts that failed, and the one that worked

**`SNES_PPU_SIMD_PIXELS=1` — all eight pixels unconditionally, two lanes at a
time on `USUB16`/`SEL`. 48.06 against 55.57: it loses 7.5 fps.** Rig-identical
picture, so it is not a bug. The per-pixel test is a **skip**, and this scene
skips constantly — 46% of tiles are blank and much of the rest transparent — so
doing two lanes unconditionally pays for every transparent pixel in the frame.

**`SNES_PPU_COARSE_SKIP=1` — one test dropping four pixels instead of four
dropping one each. 55.67 against 55.45: +0.22, inside the noise.** Which is
itself a finding: transparent pixels here are **scattered**, not clustered in
half-tiles, so there is rarely a run of four to drop.

**`SNES_PPU_VIRGIN_Z=1` — the first layer into a z-buffer skips the z test
entirely. 56.41 against 55.55: +0.86 fps, +1.5%. Shipped, on by default.**
`ClearBackdrop` fills both buffers with `0x0500` every drawn line, so until
something else writes there the compare cannot fail for any layer whose z floor
is above it. Exactly one pass per screen is the first — 2 of the 3.29 passes a
line — and it is the base layer, the one with the fewest transparent pixels.
Rig hashes bit-identical, 18,998 fewer instructions a frame.

Note the shape of the three. Removing the skip lost 7.5. Making the skip coarser
was noise. Removing the **compare** — work that was provably redundant, not work
that was merely often unnecessary — won. On this loop, delete what is *always*
useless; do not try to predict what is *usually* useless.

### What is left

The pixel work still prices at ~3.5 fps after virgin-z. What remains inside it is
the per-pixel `if (pixel)` test and the store, on passes 2 and 3 also the load and
compare. Candidates, none built:

- **The same trick for the sprite merge.** It runs `if (src[0] > dst[0])` over a
  whole span; when the buffer is virgin, that is a `memcpy` — and the code
  already has that path (`clear_backdrop`), just not driven by this flag.
- **A z floor per line rather than per layer.** If the maximum z already in the
  buffer is below this layer's floor, the compare is redundant for pass 2 as
  well. One max tracked per line per screen would extend virgin-z past the first
  pass, at the cost of a running max.

### Infrastructure, when it comes to that### Infrastructure, when it comes to that

The pixel loop is `pixel = (chunky >> 4i) & 0xf; if (pixel && z > dstz[i]) dstz[i] = z + pixel;`
eight times, on `uint16` — which is a Cortex-M7 SIMD shape. `USUB16` sets the GE
flags per halfword and `SEL` picks by them, so two pixels can be compared and
merged branchlessly per iteration, and the decode's four `PpuSpreadByteToNibbles`
are pure shift/mask work that hand-scheduling can overlap with the stores.

The infrastructure exists and should be copied exactly: `.S` under
`src/snes/thumb2/`, a `_offsets.h`, a `_offsets_check.c` full of `_Static_assert`
against `offsetof`, an entry in `SNES_ASM_SOURCES` (Makefile:158) and
`snes_redefines` applied to the object — the same shape as `snes_thumb2.S` and
`spc_thumb2.S`.

**Ceiling: 4.4 fps**, but read it as "the pixel loop costs 4.4", not "4.4 is
sitting there". The first attempt to take it gave back −7.5.

## The rule, in its final form

The core is stall-bound in the sense that instruction count does not convert to
speed one-for-one (−5.5% instructions bought +1.9%). But **memory is not the
bottleneck** — that was this session's wrong turn, and it cost four experiments.

What has won, every time: **deleting work outright**, especially work in a loop
that runs tens of thousands of times a frame.
What has lost, every time: **adding a test to skip work**, unless what it skips is
large (a BRR decode, a Gaussian interpolation) rather than a few instructions.

## Measured and closed — do not re-propose

| | fps | |
|---|---|---|
| colour-math compositing, 2 px/iteration | 47.99 | −4.8% |
| DSP idle-voice BRR skip | 48.85 | −3.1% |
| DSP idle-skip delete + channel pointer hoist | 51.72 | −0.64 (gcc had already folded the hoist) |
| pacing reference advanced by one period | 57.10 vs 57.40 | a 21 ms frame advances the tick counter by 1, same as a 14 ms one |
| whole-D-cache clean before present | 52.17 | −0.19 |
| sprite two-pass reverse draw | neutral | 8 loads removed vs 4 stores added per sliver; scene-independent ratio |
| frameskip sprite-draw skip | 52.29 vs 52.36 | 5.46 slivers/line against a limit of 34 |
| `PpuWindows_Calc` duplicate | never built | 0 of 42,191 sub passes duplicate a main layer |
| tile memo / prefetch / framebuffer / VRAM-in-DTCM | see above | the memory theory, closed — the fetch was never the cost |
| software pipeline, depth 1 / depth 2 / two-pass batched | 55.53 / 55.33 / 55.46 | all nothing; they reorder a fetch that is free |
| per-call setup (windows, tilemap base, row addresses) | 59.73 | free; keeping only it is as fast as deleting the whole draw |
| the tilemap walk (`NEXT_TP`) | 55.46 | free |
| `SNES_ROMCACHE=1` | 55.10 vs 55.47 | verdict stands, re-measured in real gameplay |
| `SNES_SPIN_SKIP=1` | 50.79 vs 55.47 | −4.68, twice what the attract screen showed |

## Knobs re-measured where the game is actually played

The line cache shipped in the wrong position because its verdict came from the
attract screen. All four were then re-checked:

| | |
|---|---|
| `SNES_LINE_CACHE` | **REVERSED** — 52.21 → 55.45 with it OFF. 1,713 hits against 27,407 misses (5.9%), and the tracking is charged on every VRAM access regardless |
| `SNES_ROMCACHE` | stands |
| `SNES_SPC_IDLE_SKIP` | stands, and this was its **first device measurement** — it rested on rig instruction counts until now |
| `SNES_SPIN_SKIP` | stands, but understated by half |

**The attract screen understates every per-opcode tax and overstates every cache.**
It is a still image that runs fewer opcodes. Measure from a savestate.

## Three gate defects fixed — all of them let a green run stand in for coverage

1. **`FLAGS_STAMP`** recorded `C_DEFS`/`CFLAGS` only. The SNES recipe adds eight
   C define groups *and* `ASFLAGS`; none were in the stamp, so toggling a knob
   rebuilt nothing and both arms of an A/B were the same binary. Fixed in two
   passes (the second because the stamp is a `$(shell)` that ran above the
   variables it needed). **Always `cmp` the two arms' `snes.bin`.**
2. **The rig's 400-frame run does not execute the tile drawers.** ALttP spends its
   first ~500 frames on a black screen: 61,376 compositing lines, **zero**
   background tiles decoded, **zero** subscreen lines. The second ROM never
   renders a tile at any length tried. Default is 1200 frames now and the rig
   prints a COVERAGE WARNING when it decoded no tile or rendered no subscreen.
3. **`coverage.sh` swallowed a link failure as a SKIP**, so `video_play.c` showed
   as "no data" rather than 19.8%.

## Facts worth keeping

- DTCM heap high-water during SNES play: **11,336 B of 90,336**. 79 KB idle.
- Render shape: main screen 2.29 layer passes/line, 65 tiles, 46% blank; subscreen
  1.00 pass, 33 tiles, **0% blank**, on 100% of lines, sharing no layer with main.
- Sprite load: 5.46 slivers per line against a limit of 34, limits never reached.
- Frame times are bimodal: 14.6 ms skipped / 32.4 ms drawn, nothing between. The
  overload guard draws one frame in four.
- Audio: at 55.47 fps the core makes 14,750 samples/s against 16,000 consumed.
  Underruns fell 126/s → 71/s with the line-cache reversal. Below 60.15 fps the
  deficit exists and no audio code can fill it.

## How to measure

- **Container decides correctness**: `run_snes_t2.sh <rom> 1200` — four hashes.
  It cannot see a cache hit and does not count `pld`; a delta of exactly zero
  means it did not run the code, not that the change is free.
- **Device decides speed**: `tools/gnw_probe/arm.sh build|flash|bench <name> …`.
  It pushes `cores/snes.bin` too — the core lives on the SD card.
- **Ablations compose, and the knobs are wired to compose**: `SNES_ABLATE_BG`
  (1 whole draw, 2 pixel work, 3 ClearBackdrop, 4 return after setup),
  `SNES_ABLATE_WALK`, `SNES_ABLATE_FETCH`, `SNES_ABLATE_ADDR`. All produce wrong
  output on purpose; the frame counter is the only valid reading.
- **Price a big lever by ablation before designing for it.** PC sampling scored
  `PpuDrawBackground_4bpp` at 6.1%; ablation said 13.7%. A sampler credits a stall
  to whichever instruction is retiring, so memory-bound work reads light.
