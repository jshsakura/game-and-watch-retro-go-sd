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

And the one avenue that was still open on paper — static recompilation — is
closed too, by work already in the tree. `docs/RC_DISPATCH_ANALYSIS.md` and
`docs/RC_PRIOR_ART.md` evaluate and reject it for this device twice over: it is
**per-ROM** (8,371 site functions for SMW alone, so it does nothing for a
generic core), the dispatch lookup costs 11–12 comparisons with heavy branch
misprediction on Cortex-M7, and the sites themselves are high-frequency indirect
branches into QSPI flash — bus waits and I-cache thrashing, which is the same
wall that killed DOOM's XIP.

**So the speed work on this core is finished**, short of hand-scheduling the
tile loop in Thumb-2 for a fraction of the 3.15 fps. What is left to decide is
an audio trade, below.

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

---

## 2026-08-12 — the pixel loop gave up one more tick, and two ways not to take it

**`SNES_PPU_OPAQUE_TILE`, shipped, on by default: 56.68 -> 57.10 fps** (+0.42),
Zelda 3 from a savestate, 900 deterministic frames, three runs each. Rig hashes
bit-identical.

A tile whose eight nibbles are all non-zero is **fully opaque**, and on it the
per-pixel `if (pixel)` is provably true eight times out of eight. Four ALU ops
decide it: OR the decoded word down by 1, 2 and 3, keep bit 0 of every nibble,
compare against `0x11111111`.

The census that justified building it (`SNES_RENDER_CENSUS=1`, new counters):

```
main   161,172 tiles decoded   38% fully opaque   5.88 opaque px/tile
sub    296,334 tiles decoded   86% fully opaque   7.53 opaque px/tile
```

The subscreen is 65% of all tile decode and 86% of it is fully opaque. That is
where the win is.

**The rig cannot see this one.** +288 insn/frame, which is nothing, with
identical hashes — because what it removes is eight *unpredictable* branches per
tile and a rig does not charge a mispredict. Every reading here is the device.

### Two negative results, both from code that never executes

The virgin-buffer branch of the same path (`flat`: fully opaque onto a z-buffer
still holding only backdrop, so no test of any kind is needed) was built and
**measured zero executions** — `flat(main/sub)=0/0`. Mode 1 draws sprites first
and `PpuDrawSprites` marks the buffer dirty, so a background pass is never
virgin on a line that has sprites. The path is kept because it is correct and
free, but nothing in Zelda 3 reaches it.

Two attempts to make that dead branch faster then cost real frames:

| | fps | |
|---|---|---|
| pair the flat stores via `memcpy(d,&v,4)` | **52.38** | **-4.7** |
| same, via an `aligned(1) may_alias` store | 56.42 | -0.8 |
| eight plain `strh` (shipped) | **57.21** | |

The first is the sharper lesson: gcc does **not** fold a 4-byte `memcpy` with an
unknown-alignment destination into a `str`. It emits a call, and seven `bl
memcpy` landed inside `PpuDrawBackground_4bpp`. **A call in the hottest loop
makes every caller-saved register clobbered across it**, and the loop that pays
for that runs tens of thousands of times a frame — while the branch containing
the call never runs at all.

The second says the rest: even with no call, merely *having* the extra branch in
that loop costs 0.8 fps in register pressure and code size. **In this loop, code
that never executes is not free.** Check `objdump -d build/snes/ppu.o` for `bl`
inside the drawer before trusting any edit to it.

### The same trick in the 2bpp drawer: closed, with the number

**56.90 against 56.95 fps**, three runs each — fully inside the spread. Not
shipped.

The rig had already explained why, and the explanation is worth more than the
result: with `SNES_RENDER_CENSUS=1` every 2bpp counter reads **zero**. Not "0%
opaque" — zero tiles decoded. BG3 is not enabled in the window the rig runs, so
there was no stimulus to price, and the device scene barely drives it either.

In a loop where an unexecuted branch was just measured at −0.8 fps, a path that
measures nothing is a liability, not a neutral. It was removed rather than left
behind a default-off knob. `g_t2_full` / `g_t2_mixed` stay under the census flag
for whoever finds a scene that does drive BG3 — mode 0 games, or a HUD-heavy
one. Price it there first; do not re-derive this.

---

## 2026-08-12 — the APU cell is no longer blank, and the next 5 fps is in it

The price table carried "APU as a whole — **unmeasurable**, `SNES_ABLATE_APU=1`
hangs the SPC boot handshake". That diagnosis was wrong, and the cell is now
filled with a verified number.

**It is not the handshake.** The frame loop drains the DSP with
`while (dsp->sampleOffset < 534) apu_cycle(apu);`, and `dsp_cycle` is the only
thing that advances `sampleOffset`. An `apu_cycle` that returns early does not
stop the game — it stops *that loop*, on frame one, forever. Resuming from a
savestate also puts the handshake in the past, so neither objection survives.

Two more traps had to be walked through before the numbers were real, and both
are the same trap the layer-loop session named — **check what the ablation
compiles to, not what its name says**:

1. `dsp_cycle` has **two callers**. The guard went in `apu_cycle`; the one the
   frame loop actually drives is `apu_run` (via `snes_catchupApu`). Guarding one
   door priced the whole APU at **+1.62 fps**. Guarding both: **+2.65**.
2. Faking `sampleOffset` inside `apu_cycle` to make the drain loop terminate is
   worse than useless when only the DSP is ablated: the SPC still runs, so the
   loop then executes **17,088 `apu_cycle` calls a frame** and the DSP measured
   as **COSTING 1.5 fps to delete** (55.55 against 57.10). The loop is stopped
   at its own call site now.

Every arm below was checked to be a running game before its number was kept —
`snes` a valid heap pointer, `g_snes_drawn_frames` advancing. An earlier arm
read **405 fps** because it was not running and `bench.sh` had fallen back to a
symbol that was not the frame counter.

### The ladder, Zelda 3 rain from a savestate, 900 deterministic frames

| ablation | emulated fps | drawn fps | Δ drawn |
|---|---|---|---|
| baseline | 57.26 | 15.92 | — |
| SPC700 only | 57.22 | 18.50 | +2.58 |
| **DSP only** | 59.68 | 21.00 | **+5.08** |
| whole APU | 59.75 | 27.75 | +11.83 |

**Emulated fps saturates at ~59.7** — both DSP and whole-APU arms land there,
just under the 60.15 audio-DMA cap. From here the emulated frame counter cannot
see an improvement at all; **drawn fps is the only remaining signal**, and every
future lever has to be judged on it.

That the parts do not add to the whole (2.58 + 5.08 < 11.83) is the overload
guard's feedback: it is a threshold, not a gradient, and each saving moves the
loop further past it.

**So the next 5 fps is the DSP**, priced at +5.08 drawn.

### What is already closed inside it — re-judged on the right metric

The two idle-voice knobs were shelved on emulated fps, which is exactly the
metric that cannot see a change to work that runs on *skipped* frames. Re-judged
on drawn fps against the same-session baseline (15.92):

| | emulated | drawn |
|---|---|---|
| `SNES_DSP_IDLE_HOIST=1` | 57.30 | 16.08 (+0.16, noise) |
| `SNES_DSP_IDLE_SKIP_VOICE=1` | 56.84 | 15.33 (**−0.59**) |

The old verdicts stand. This was worth re-running: the hypothesis that they had
been judged blind was reasonable and is now disproved with the instrument that
would have seen it.

Also checked and already done: `dsp_handleEcho` early-outs when echo can affect
neither the output nor ARAM. And the mono downmix **averages** L and R, so
folding the per-voice stereo mix into one multiply is not free — the per-channel
16-bit clamps make it non-equivalent, and any attempt must clear AUDIOHASH on
the ROM corpus first.

### Sparse echo FIR — CLOSED, and it produced a fake +10.5 fps first

The echo ablation says the FIR is worth **+3.08 drawn fps**, and SNES games
often leave most of the eight FIR coefficients at zero, so skipping zero taps
looked like the shape that keeps winning here: work that is *always* useless.

It is not, and two things went wrong in the order they are worth remembering.

**1. The census kills it, and it should have been run first.** With
`SNES_RENDER_CENSUS=1` the rig reports **8.00 non-zero taps of 8** on Zelda 3 —
there is nothing to skip. The rig also priced the change honestly: **+52,558
instructions a frame** for a mask test that never fires. "Price a lever by
ablation before designing for it" applies to the *sub*-lever too; the echo being
worth 3.08 says nothing about which part of the echo is dead.

**2. The device said +2.44 emulated and +10.5 drawn fps, and that was a
different SCENE.** (Corrected below — the first diagnosis, "corrupt state does
less work", was wrong in its mechanism.) The mask was first stored as a new `Dsp` field, placed
after `firValues`. `dsp_saveload` writes `sizeof(Dsp) - offsetof(Dsp, ram)`
bytes as a **raw struct dump**, so a field added anywhere in that span shifts
every field after it — and the measurement arms resume a savestate written by a
build without it. The DSP was restored to nonsense, stopped doing work, and the
frame counter rose. Bracketed A/B/A reproduced it three times; the rig never saw
it because the rig cold-boots.

CLAUDE.md already says this in as many words ("a savestate is a raw dump of live
structs... yesterday's file still opens, still reads to the end, and quietly
restores nonsense"). What is new is the failure mode: **a savestate mismatch can
look exactly like a large win**, because corrupt state does less work. Any arm
that resumes a savestate and shows a big gain must be checked against a struct
change before it is believed.

Derived state belongs in a file-scope static, next to `g_dsp_idle_mask`, and has
to be rebuilt after a load. Moving it there and re-measuring gave a build that
did not run at all — at which point the census had already settled the question,
and the whole change was reverted.

### Inside the voice — first numbers, because three knobs were never wired

`SNES_ABLATE_DSP_BRR`, `_INTERP` and `_GAIN` existed in `dsp.c` with **no
makefile wiring**, so they could not be turned on. Anything ever "measured" with
them measured the baseline — the same defect `SNES_ABLATE_BG=2` had, which the
issue records as the origin of a wasted session. Wired now.

Census first this time (`SNES_DSP_CENSUS=1`, ALttP 900 frames): 3,844,800
voice-ticks, **59.5% idle**, 1,555,666 active, **76,836 BRR decodes** (85 a
frame), and **zero** pitch-modulated voices.

Device, against 57.2 emulated / 16.1 drawn:

| deleted | emulated | drawn | Δ drawn |
|---|---|---|---|
| BRR decode | 55.80 (**−1.4**) | 18.33 | **+2.2** |
| Gaussian interpolation | 55.70 (**−1.5**) | 17.33 | **+1.2** |
| envelope / gain | 57.49 | 16.08 | **0.0** |

The first two show the signature this issue named: **emulated fps falls while
drawn fps rises**, because cheaper skipped frames let the overload guard draw
more. Judged on fps alone both would read as losses.

**Read the first two as contaminated upper bounds.** Deleting the BRR decode
leaves the decode buffer stale and the interpolation reads it, so voices go
quiet and downstream work disappears with them — the ablation changes the
emulated state, not just the code. Only the envelope's **0.0** is a clean
verdict, and it is a real one: the ADSR/gain path is free.

What is left with a number and no implementation: the four-tap Gaussian
interpolation, +1.2 drawn, which is an `SMLAD` shape on this core. Anything
built there has to clear AUDIOHASH on the ROM corpus — the SNES DSP's clamping
is defined per step and a wider accumulator does not reproduce it for free.


#### Correction: the savestate stamp worked. The autoboot was the silent part.

The paragraph above blamed a raw struct dump restoring nonsense. That is not
what happened, and the difference matters because it exonerates a mechanism that
is doing its job.

`snes_state_header_t` carries **magic, version AND payload length**, and the
loader refuses on all three *before touching the machine*. Adding a field to
`Dsp` changes `sizeof(Dsp)`, so the payload length changed, so the state was
**correctly refused**.

What was silent is the caller. `app_main_snes` did:

```c
if (load_state) odroid_system_emu_load_state(save_slot);
```

— the result dropped. A refused state starts the ROM **cold**, so the arm was
benchmarking Zelda's title screen while every other arm benchmarked the rain.
The title screen is much cheaper. That is the whole +10.5 drawn fps.

So the rule to carry is not "beware raw struct dumps" (this one is stamped and
refuses). It is: **an arm that resumes a savestate must prove it resumed.**
`g_snes_state_resumed` is published for that, `arm.sh` reads it over SWD before
it will report a number, and the firmware prints `savestate refused ... running
COLD`. Any struct change anywhere in the state trips this, so it will fire more
often than it seems it should.

### The ceiling, measured: emulated fps cannot win, drawn fps can

Freeze the emulation and see what the loop does. All verified running, savestate
confirmed resumed:

| | emulated | drawn |
|---|---|---|
| baseline | 57.20 | 16.1 |
| 65816 frozen | 59.70 | 36.4 |
| APU frozen | 59.75 | 27.8 |
| **both frozen** | **60.03** | **60.25** |
| audio-DMA cap | 60.15 | — |

**The whole emulation — interpreter and APU together — is worth +2.83 fps**, and
the distance to the cap is +2.95. Reaching 60 on the emulated counter therefore
requires making the emulator very nearly free. It is not a matter of finding one
more lever; the sum of every lever that exists is the budget.

Two things fall out of the bottom row.

**The render is not an independent item.** With both frozen, rendering is fully
on and every frame is drawn (60.25 drawn), and the loop still reaches 60.03. The
PPU is only expensive because the CPU keeps changing the scene; "remaining
render, 3.15 fps" is a number that sits on top of the CPU's, not beside it.

**Drawn fps is where the win is.** 16.1 today against 60.25 with the emulation
gone — the overload guard is holding four fifths of the frames back, and it
releases as soon as the frame fits. Every lever measured this session moves
drawn fps several times as far as it moves the emulated counter, and drawn fps
is what a player sees.

### Boot clock: level 3 is dead code

`SystemClock_Config(3)` claims 353.7 MHz (PLLM=38, PLLN=420). The ELF shows
`movs r0, #3` reaching the call, and the PLL registers afterwards read
**PLLM=16, PLLN=170, PLLP=2 — level 2, 340.0 MHz**. So the call silently lands
on the shipping clock and reports nothing. Not pursued (a 4% clock is not the
answer and the battery cost is real), but recorded: anyone reading that switch
would reasonably assume level 3 works, and it does not.

Also confirmed while looking: **the shipping overclock IS working.** The PLL
reads 340.0 MHz core / 97.1 MHz OSPI, which is level 2, not the 280 MHz stock
clock. `SystemCoreClock` agrees. `ENABLE_BOOT_OC=1` does what it says.
