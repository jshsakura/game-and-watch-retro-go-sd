# SNES — where this stands, and what to aim at next

Rewritten 2026-08-11. Everything here is measured on hardware unless it says
otherwise: Zelda 3 rain, resumed from a savestate so every arm starts in the same
scene, 900 deterministic frames, three to six runs per arm.

## State

| | |
|---|---|
| Real-gameplay baseline | **55.47 fps** (from 50.39 at the start of the session, **+10.1%**) |
| Shipped release | `testbed-full-20260810-0136` — built at 52.36, **predates the line-cache reversal**, so a new release is worth cutting |
| Branch | `testbed`, submodule `external/sm` on `perf/spc-idle-skip`, both pushed |
| Device | idle power-off is compiled out of every arm now (`GNW_NO_IDLE_OFF=1` in `arm.sh`) |

## The next target

**The layer-draw loop skeleton — not its memory, not its arithmetic.**

Two ablations bracket it and a third set of experiments identifies what is left:

```
delete decode + z-compare + store          55.46 -> 55.46   nothing
delete the whole layer draw                55.46 -> 59.80   +4.33
```

The difference is 4.33 fps, and it is **not** the VRAM reads. Four independent
attacks on read cost all measured zero:

| | fps |
|---|---|
| tile memo, 80% hit rate on consecutive tilemap entries | 55.60 |
| PLD prefetch of the next tile's bitplanes | −0.37 |
| framebuffer's 155 KB removed from the D-cache | 55.58 |
| **all 64 KB of VRAM moved to zero-wait DTCM** | **55.43** |

The last one is decisive: it removes every cache miss the renderer can suffer
(verified by reading `ppu->vram` over SWD as `0x20006a70`, so it allocated rather
than falling back) and returns nothing.

So what is left between the two ablations is the loop itself:

- `NEXT_TP()` — tilemap pointer advance with its wrap branch, 68 times a line
- the window-span loop and its clipping arithmetic, per layer per line
- the per-layer call into `PpuDrawBackground_4bpp` / `_2bpp`

**That is control flow, and control flow is what has actually paid on this part.**
All four levers that won today were control flow: deleting a call frame, folding
a per-access charge into one per opcode, deleting a per-pixel range test, and
compiling out a dead per-opcode branch.

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
| tile memo / prefetch / framebuffer / VRAM-in-DTCM | see above | the memory theory, closed |
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
- **Price a big lever by ablation before designing for it.** PC sampling scored
  `PpuDrawBackground_4bpp` at 6.1%; ablation said 13.7%. A sampler credits a stall
  to whichever instruction is retiring, so memory-bound work reads light.
