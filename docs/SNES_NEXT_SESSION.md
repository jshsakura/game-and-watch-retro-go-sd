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

**A hand-scheduled Thumb-2 inner loop, or nothing.** The layer draw was taken
apart this session and it is a **pointer chase**, not memory and not arithmetic:

```
baseline                                     55.57   (55.38 on the end-of-session recheck)
return before the span loop                  59.73   +4.16
bitplane load replaced by ALU arithmetic     58.61   +3.04
loads kept, address not from the tilemap     57.66   +2.09
delete decode + z-compare + store            55.46   nothing
delete the tilemap walk (tp never advances)  55.46   nothing
software pipeline, depth 1                   55.53   nothing
software pipeline, depth 2                   55.33   nothing
```

Read it in three steps.

1. **The per-call setup is free.** Keeping only the enabled/windowed tests,
   `PpuWindows_Calc`/`_Clear`, the tilemap base and the two row addresses — and
   returning before the span loop — gives back 4.16 fps, which is everything
   deleting the entire draw gives back. The caller is not the target.
2. **Inside the loop, only the fetch matters.** The pixel work prices at zero and
   the walk prices at zero; the bitplane fetch prices at 3.04 of the 4.16.
3. **And the fetch is a chain, not a load.** Keep both loads, at the same rate,
   scattered over the same 64 KB, but stop deriving their address from the
   tilemap word, and 2.09 of the 3.04 comes back. The cost is `load a tilemap
   word → its low bits pick an address → load that address → branch on it`.

That is also the retrospective explanation for last session's four zeroes. A
chase of L1 hits is not a cache problem, so the tile memo (80% of fetches gone),
the PLD, the framebuffer eviction fix and all 64 KB of VRAM in zero-wait DTCM
were each attacking a stall that was never there.

**And C cannot fix it.** Software pipelining is the textbook answer — put a whole
iteration of other work between each load and its use — and both depths were
built, verified bit-identical in the rig, and measured at nothing. Depth 2 costs
5,186 more instructions a frame in the rig: gcc paid for the extra live values in
spills and re-serialised what the source had spread out. Both are behind
`SNES_PPU_PIPELINE` and both are off.

So the remaining 4.16 fps in the renderer is reachable only by scheduling the
loop by hand in Thumb-2, where the load issue order is not the compiler's to
undo. That is the honest price of that project: **4.16 fps**, not the 7.18 the
first ablation suggested, and not zero.

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
| tile memo / prefetch / framebuffer / VRAM-in-DTCM | see above | the memory theory, closed — and now explained: a chase of L1 hits |
| per-call setup (windows, tilemap base, row addresses) | 59.73 | free; keeping only it is as fast as deleting the whole draw |
| the tilemap walk (`NEXT_TP`) | 55.46 | free |
| software pipeline, depth 1 and depth 2 | 55.53 / 55.33 | the right idea, undone by the compiler |
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
