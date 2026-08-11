# SNES — where this stands, and what to aim at next

Rewritten 2026-08-11, then updated the same day after the layer loop was taken
apart. Everything here is measured on hardware unless it says
otherwise: Zelda 3 rain, resumed from a savestate so every arm starts in the same
scene, 900 deterministic frames, three to six runs per arm.

## State

| | |
|---|---|
| Real-gameplay baseline | **55.47 fps** (from 50.39 at the start of the session, **+10.1%**) |
| Shipped release | **`testbed-full-20260811-1355`** — the first release carrying the line-cache reversal (CI green) |
| Branch | `testbed`, submodule `external/sm` on `perf/spc-idle-skip`, both pushed |
| Device | idle power-off is compiled out of every arm now (`GNW_NO_IDLE_OFF=1` in `arm.sh`) |

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

### First attempt, and what it teaches

`SNES_PPU_SIMD_PIXELS=1` does the eight pixels two at a time on the M7's
halfword SIMD — `USUB16` sets the GE flags per lane, `SEL` picks by them, and
the transparent case folds into the same compare by substituting 0 for `z`.
Rig hashes bit-identical. **Device: 48.06 against 55.57 — it loses 7.5 fps.**

The reason is the thing it deleted. `if (pixel && z > dstz[i])` is a test that
**skips**, and this scene skips constantly: 46% of tiles are blank outright and
much of the rest is transparent. Two lanes unconditionally means paying for
every transparent pixel in the frame.

So the 4.4 fps is **not** eight pixels of arithmetic waiting to be vectorised. A
large part of it is the skipping machinery itself, and anything that replaces a
skip with unconditional work loses — this project's own rule, arriving from the
other side.

### What to build

**A coarser skip.** One test that drops four pixels at once (`(chunky & 0xffff)
== 0`) rather than eight tests that drop one each. That keeps the skip — which
is clearly earning its keep — and cuts the per-pixel overhead of applying it.
Same shape as the DSP idle fast paths, which won for the same reason: what they
skip is large.

Only after a coarse skip pays is hand-written Thumb-2 worth it, and then its job
is the *skip decision*, not the arithmetic.

### Infrastructure, when it comes to that

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
