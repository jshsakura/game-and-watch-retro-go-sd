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

### Re-judging the shelved knobs on drawn fps — one flips

Three knobs were closed on emulated fps, which the ceiling above shows cannot
report an improvement. Re-run on drawn fps, savestate resume verified:

| | emulated | drawn | old verdict |
|---|---|---|---|
| `SNES_SPRITE_SKIP_DRAW=1` | 56.53 (−0.7) | 16.42 (+0.2) | stands — still nothing |
| **`SNES_ROMCACHE=1`** | 55.71 (**−1.22**) | **18.25 (+1.96)** | **reversed** |
| `SNES_LINE_CACHE=1` | 53.38 (−3.9) | 14.17 (−1.9) | stands, and worse than recorded |

`SNES_ROMCACHE` was closed as "55.10 against 55.47, verdict stands, re-measured
in real gameplay". That measurement was on the blind counter. Bracketed A/B/A on
drawn fps: **16.33 → 18.25 → 16.25**, +12%.

**It is a trade, not a win, and the trade is not ours to make.** The knob makes
CPU work cheaper; the overload guard converts every bit of that into drawing
more frames, automatically, and the emulated rate falls by 1.22 as a result.
Emulated fps is what feeds the audio: the sample deficit goes from **5.4% to
7.4%**, so the rain crackles harder. Smoother picture, worse sound, and no way
to ask for the other split — the guard decides.

**Not shipped on.** It belongs next to `SNES_STRETCH_FOLLOW` as something a
listener chooses, or behind a "prefer smoothness / prefer sound" setting, if
anyone wants to offer the choice at all.

This is the second knob this session whose verdict came from a metric that could
not see it. Anything closed with an emulated-fps number before today should be
assumed unjudged.

### The smoothness/sound trade is a cliff, and the shipping build sits on its edge

The overload guard's forced draw — `skip_streak >= 4`, one frame in four — was a
floor so the screen could not go blank, not a number chosen for any workload. On
SNES the integrator is permanently pinned, so that constant **is** the draw rate.
Measured (`GNW_FORCED_DRAW_RATIO`), savestate resume verified, underruns read as
a rate rather than a total:

| forced draw | drawn fps | emulated fps | **audio underruns** |
|---|---|---|---|
| **1 in 4 (shipping)** | 16.08 | 57.17 | **0 /s** |
| 1 in 3 | 18.67 (+16%) | 55.50 | 19 /s |
| 1 in 2 | 25.42 (+58%) | 50.54 | 478 /s |

**Not a curve — a cliff.** Underruns are exactly zero at 1-in-4, start at
1-in-3, and collapse at 1-in-2. The constant nobody chose turns out to be the
last position where the audio still closes.

That settles two things. **The draw ratio stays at 4.** And `SNES_ROMCACHE`
stays off: it buys the same +2 drawn fps by spending the same emulated fps, and
there is no audio budget left to spend.

It also says what a real optimisation has to look like from here. Anything that
buys smoothness with emulated fps is already at its optimum — the guard is
sitting on the edge. **Only a change that raises emulated fps moves both axes**,
because the guard then converts the surplus into drawing on its own. That is
what the opaque-tile path did (+0.42 emulated, +0.6 drawn, both) and what every
trade cannot do.

### WRAM before the ROM tag — closed, and the census that justified it was measured on the wrong machine

`snes_cpuRead` tests the ROM page tag first. A rig census said **7,058 of 7,829
bus reads a frame are WRAM**, which would make that tag test a question whose
answer is no nine times in ten. Reordering is free by construction — the
classifications are disjoint, only the ROM branch ever installs a tag — and the
rig agreed: bit-identical hashes, **−43,163 instructions a frame (−1.12%)**.

On hardware, bracketed A/B/A: **16.33 → 14.50 → 16.17 drawn**, −0.2 emulated,
and the first audio underrun this scene has produced at the shipping settings.

**The census was taken in a configuration the device does not run.** The rig
builds `snes_thumb2.S` without `-DSNES_T2_NO_ROMCACHE` (that define is passed
through `ASFLAGS`, which `run_snes_t2.sh` does not set), so in the rig the
engine's **inline ROM page cache is ON** and opcode fetches never reach
`snes_cpuRead` at all. The device ships `SNES_ROMCACHE=0`, so every fetch goes
through it — and fetches are the majority. Putting WRAM first bought nine tenths
of a minority and charged every fetch two dead tests.

Two things to carry:

- **`run_snes_t2.sh` does not reproduce the device's ROM-cache setting.** Any
  census of the bus taken there is measuring a different mix. The engine's
  `ASFLAGS` defines are not part of what `RIG_EXTRA_DEF` reaches.
- The instruction count was right and pointed the wrong way, one more time.
  −1.12% of instructions bought −1.7 drawn fps.

### The true bus mix, and the combination that does not work

With the rig finally assembling the engine the way the device does
(`-DSNES_T2_NO_ROMCACHE`, now the default in `run_snes_t2.sh`), the bus census
inverts:

```
reads 34,475 /frame    ROM 27,335 (79%)   WRAM 7,058 (20%)   slow 81
opcodes 13,220         -> 2.6 reads per opcode
```

The broken census counted 7,829 because opcode fetches never reached
`snes_cpuRead` in that build. ROM is the majority, by four to one.

**`SNES_ROMCACHE=1` + a lower draw ratio: closed.** The reasoning was that the
cache is a real CPU saving which the guard spends on drawing, so drawing less
should hand it back as emulated fps. It does not:

| | emulated | drawn | underruns |
|---|---|---|---|
| baseline (cache off, 1-in-4) | 57.02 | 16.2 | 7–9 /s |
| cache on, 1-in-5 | 56.05 | 15.50 | 32 /s |
| cache on, 1-in-6 | 56.84 | 13.33 | 7 /s |

Worse on every axis at 1-in-5, and 1-in-6 only loses drawn frames without
returning emulated ones. Whatever the cache's +1.96 drawn at ratio 4 is, it is
not a surplus the guard is redistributing.

**Caveat on the cliff above.** The same baseline build read **0 underruns/s** in
one window and **7–9/s** in another. The direction of the draw-ratio cliff
(1-in-3 → 19/s, 1-in-2 → 478/s) is far outside that, but "exactly zero at
1-in-4" was one window's luck, not a property. Underruns need the frame-aligned
window (`stretch_ab.sh`) to be compared at this resolution, exactly as the
audio work established earlier — a wall-clock read is not the same window twice.


### The baked wait loop: the prize is real, the model was missing the tax, and the replay design was unsound

`SNES_SPIN_BAKE` recognises the wait loop in the cartridge image at load and
**executes** it, instead of learning a pattern at runtime and replaying it. Four
bytes are the whole signature:

```
806b: a5 10     LDA $10        ; direct page, low WRAM
806d: f0 fc     BEQ $806b      ; back to itself
```

Three things this settles, each of which contradicts the plan it came from.

**The handoff's charges were the wrong rig's.** `charge 0/6` came from
`rig_snes_spin.c`, which is the C interpreter and computes
`(cycles - memOps) * 6`. The device engine computes `cycles*6 + memOps*2` and
the same loop is **24/22**. `rig_snes.c` now prints SPINPAT so the number comes
from the engine the device runs. (Its `[spin]` counters had been nested inside
`#ifdef RIG_COST_PROF`, so a `-DSNES_SPIN_SKIP` run printed no spin line at all;
both prints now sit outside every other guard.)

**"Bake the pattern and replay it" cannot work, and it fails fast.** The
learner's `spin_note()` on every real opcode IS the honesty mechanism -- it
drops the pattern the moment real execution diverges, and that is what the
4.78 fps buys. Strip the learner and keep the replay and the loop can no longer
see the byte the NMI handler wrote. Measured, `SNES_BAKE_BLIND_REPLAY=1`:
`STATEHASH=74d314ee` against the correct `eb1a2262`, and **faster** -- the shape
of a win that is a different machine. The shipping path instead reads the polled
byte out of WRAM, sets A/Z/N exactly as the interpreter would, and takes the
branch on that Z, so there is no purity proof to maintain and nothing to watch.
Bit-identical state AND audio hashes on three ROMs.

**Net = benefit - a fixed tax, and the tax does not care about spin share.**
Priced by a third arm that compiles the mechanism in and installs nothing:

| ROM | laps/frame | tax | net (rig insn/frame) |
|---|---|---|---|
| Super Mario World | 3,643 | | **-7.67%** |
| A Link to the Past | 730 | +118,518 (+2.39%) | +0.30% |
| Super Mario Kart | **0** | +2.8% | **+2.8%, all tax** |

Benefit is 142 insn per replayed lap, so break-even is ~835 laps/frame. Mario
Kart matches the signature at `$c0:805c` and **never executes it** -- it pays
the tax for nothing. `prize = 2.5 fps x pure%` was missing this term entirely.

The tax is not the compare. Written inline in `run_dots` the body cost ~12
instructions per opcode by perturbing that loop's register allocation (the DMA
path picked up a reload per cycle); split behind a call it is 118,518/frame, and
forced out of line into a dispatch helper it is **249,892**. Placement was worth
more than the algorithm.

**Device, Zelda savestate scene, bracketed A/B/A:** emulated 57.26/57.29/57.12
-> 56.91/56.90/57.21 -> 56.97/57.51/57.35, i.e. **-0.24 fps** -- and drawn
frames, which is the instrument that matters, went the other way:

| arm | draw ratio | drawn fps |
|---|---|---|
| off | 0.2800 / 0.2838 / 0.2804 / 0.2822 (bracket) | 16.38-16.52 |
| **on** | 0.3252 / 0.3273 / 0.3261 | **18.87-19.04** |

**+15.8% drawn (16.44 -> 18.95, +2.5 fps)**, the same emulated-down/drawn-up
trade `SNES_ROMCACHE` showed. Measured with `tools/gnw_probe/drawn_ab.sh`
(reset- and frame-aligned; it repeats to 0.9% where a wall-clock window spread
7.3 fps).

**The rig called this ROM a wash, and the rig was wrong, for a reason that
invalidates the other rig verdicts too.** The play scene replays **3,976
laps/frame**; the rig's cold-boot window replays 730. A 5.4x understatement of
the only quantity the whole model depends on. So "Zelda is below break-even"
was an artifact of the scene, and **Mario Kart's laps=0 -- also a cold-boot
window -- cannot be read as "this ROM never spins" either.** Anything judged on
a rig boot window has to be re-judged on a savestate scene before it counts.

**Device, SMW: not measurable today.** Both arms sit pinned at the ~60.0 fps
audio cap, so the prize can only appear as drawn frames -- and SMW's slot-0
savestate is refused, leaving only the free-running attract scene, where the
SAME build reads 20.73, 25.27, 21.70, 28.04, 24.40, 26.22 and 25.12 drawn fps.
`tools/gnw_probe/drawn_ab.sh` (frame-aligned, reset-aligned, and it names the
scene on every line) still reads 0.3875 and 0.4168 on one build. The scene moves
faster than the alignment can pin it; the measurement needs a savestate.

**Mario Kart, on the device, also replays nothing.** `g_bake` reads
`pc=c0:805c/805e dp_off=44 sites=1 laps=0` in the scene autoboot can reach --
the signature is in the ROM and the loop is not executed. So the "pays the tax
for nothing" case is real on hardware, not just in the rig.

**Both stale savestates are stale by exactly four bytes.** `g_snes_state_refuse`
reads stage 5 (payload length) for SMW (269,355 vs 269,359 expected) and for
Mario Kart (269,479 vs 269,483). Those four bytes are the `cpuMemOps` slot that
`d0e0ffb perf(snes): charge the bus once per opcode` added inside the
`hPos..openBus` block -- the MIDDLE of the stream, so the files cannot be
rescued by padding. Zelda's state postdates it and loads. The consequence for
measurement: the only ROM whose play scene is reachable today is Zelda.

### Super Mario World, on hardware, on a scene the console made for itself

SMW's slot-0 state was four bytes short, nobody can sit at a console with a
debug probe soldered to it, and a free-running attract demo spread the SAME
build over 7.3 drawn fps. So the console writes its own scene:
`GNW_AUTOSAVE_FRAME=<n>` boots the ROM, runs n deterministic frames and saves
slot 0 once, through the same call the menu uses. Every later arm resumes
exactly there. The file it wrote is 269,359 bytes of payload -- the length this
build streams, against the stale file's 269,355.

Bracketed A/B/A on that scene, 1800 emulated frames per sample:

| arm | drawn fps | draw ratio |
|---|---|---|
| off | 24.86 / 23.33 / 23.14, then **23.19 / 23.10 / 23.18** returning | 0.379-0.408 |
| **on** | **28.28 / 27.99 / 27.97** | 0.458-0.463 |

**+21.2% drawn frames (23.16 -> 28.08, +4.9 fps).** The armed arm's worst
sample beats the disarmed arm's best. Emulated fps is 60.08 against 60.00 --
both pinned on the audio cap, which is exactly why emulated fps could never
have shown this and drawn fps had to.

### Where it ended: the test moved out of the loop, and the regressions went with it

The per-opcode guard was the whole problem, and the fix was not to make it
cheaper but to stop asking per opcode. A wait loop is entered once and spun
thousands of times, so `run_dots` asks once per SPAN whether the pc is in the
loop, and if it is, `spin_bake_run_span()` replays laps until the loop breaks —
a copy of run_dots' own body with the interpreter dispatch replaced. Two details
carried most of the numbers:

- **Entering mid-opcode is the common case.** The first version returned when
  `cpuCyclesLeft != 0`, which is how spans usually start; it replayed 33 laps a
  frame where the per-opcode version replayed 3,643.
- **A burst of HDMA must not end the replay.** A Link to the Past's rain runs
  HDMA every scanline, and without re-entering when `dma_active` falls the win
  was 9.4% instead of 17.2% drawn. The test on that edge is once per burst.

Final, on hardware, on the console's own savestate scenes, bracketed:

| ROM | off | on | |
|---|---|---|---|
| Super Mario World | 23.46 | **33.33** | **+42.1%** |
| A Link to the Past | 16.44 | **19.15** | **+16.5%** |
| Super Mario Kart | 16.95 | 17.25 | +1.8% |
| Super Metroid | 11.41 | 11.42 | **0.0%** |

**Nothing regresses.** Mario Kart's −10.5% became +1.8%, and Super Metroid —
which contains **no match at all** (`on=0, sites=0`) — pays nothing measurable.
Getting that last cartridge to zero took one more step, and it is the same
lesson a third time: the span replay used to hand the span back when DMA went
busy, so run_dots had to ask on every DMA cycle whether the pc was back in the
loop. That question cost **1.1% of Super Metroid's frame** — for a loop that
cartridge can never run. Behind a loop-invariant local it was still 0.6%. The
answer was not a cheaper test but **no test**: the span replay runs the DMA
burst itself, copied from run_dots line for line, and the re-entry disappears.

Every arm is bit-identical to its baseline on the rig's state AND audio hashes.

### What it looked like before the test moved (kept: every number here was paid for)

### All four cartridges on the card, on scenes the console made for itself — and half of them lose

`GNW_AUTOSAVE_FRAME` made a fresh, resumable scene for every ROM, so this is
four play scenes, not one play scene and three title screens. Drawn frames,
1800 emulated frames a sample, bracketed:

| ROM | off | on | |
|---|---|---|---|
| Super Mario World | 23.16 | **28.08** | **+21.2%** |
| A Link to the Past | 16.44 | **18.95** | **+15.8%** |
| Super Metroid | 11.41 | 11.09 | −2.7% |
| Super Mario Kart | 16.81 | 15.05 | **−10.5%** |

**This cannot ship as it stands.** Two carts gain a sixth to a fifth of their
drawn frames and two lose, one of them badly. And the mechanism is high-gain in
BOTH directions for the same reason: Mario Kart's emulated fps falls 1.5% and
its drawn frames fall 10.5%, the mirror image of Zelda turning −0.24 emulated
into +15.8% drawn. Near the overload guard's pinning point a small change in
emulation time is a large change in what the player sees.

The cause is one sentence: **the guard runs on every opcode and only a spinning
cart is ever repaid.** Super Metroid pays ~2.7% for a loop it does not run.

The fix has a place to go. The Thumb-2 engine dispatches through a dense table
with a dedicated `.Lopa5` handler for `LDA dp` — the first of the loop's two
opcodes. A test there runs only on LDA-dp opcodes instead of all of them, which
is where a test belongs: next to the information it needs. That is assembly work
on `snes_thumb2.S` and it is the next session's, not this one's.

### The gate that had to be rebuilt, because the obvious one was worse than the disease

The first gate specialised `run_dots`/`run_frame_events` into `baked` and
`plain` clones and chose per span, so a disarmed cart would fold the guard away
entirely and pay nothing. It does not work, and the failure is not subtle: a
build that INSTALLS NOTHING drew **14.16 fps against the baseline's 16.44 on
Zelda -- 13.9% lost to code that never runs**. Instantiating both clones is
enough to change what gcc does with the frame loop; the disarmed clone came out
64 bytes smaller than the baseline's and 2.2% slower. Three placements were
measured (per-span branch, per-frame branch, armed clone isolated behind
`noinline`) and all three cost about the same. The one build that matched the
baseline -- 3,764,334 insn/frame against 3,764,322, a difference of twelve --
was the degenerate one that instantiates a single clone.

**What ships is one code path that disarms by moving the pc out of reach**:
`pc_load = 0xffff`, which the compare can never see. The guard's cost stays;
only the replay stops. A cart that does not spin pays the compare (~2.4% of a
frame) rather than a different program (13.9% of its drawn frames), and the
window/park cycle retries rather than deciding once -- because spin rate is a
property of the scene, not the cartridge, as Zelda's 730-vs-3,976 proves.

**Two traps caught on the way.** Regenerating `snes_redefines` **deleted 89
symbols**, because the generator compiles the core without `-DSNES_THUMB2_CPU`
and friends, so those symbols do not exist in its objects -- and the next link
failed on `multiple definition of snes_cycles_per_opcode` against the SM
overlay, the exact collision that file prevents. It merges now, never replaces.
And the savestate loader refused a file while saying only "refused": the file
turned out to be well-formed, current-version and self-consistent, after two
wrong guesses. `g_snes_state_refuse[4]` now carries stage/file/expected/size.

---

## Start here next session

**Device state:** the shipping default build is flashed (`gapfree` arm), 57.2 fps,
savestate resume verified. `tools/gnw_probe/arm.sh build|flash|bench <name> …`
refuses to report a number from an arm that booted cold.

**One command, and the question it settles**

```bash
bash tools/gnw_probe/arm.sh build spinpat GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=0 \
     SNES_SPIN_SKIP=1 SNES_SPIN_REPLAY_ONLY=1     # + the baked pattern
```

Bake Super Mario World's wait loop (`pc 0x00806b/0x00806d`, signature
`A5 10 F0 FC`, charges `0/6`), put an `$00:806d` ROM on the card, and A/B it.
That signature covers 51 ROMs and the model says 1.78–2.06 fps for them. It also
tests the model itself: **prize ≈ 2.5 fps × pure%**, which is an instruction
argument, and instruction arguments pointed the wrong way three times today.

**What is already settled, so it is not re-derived**

- **60 fps is unreachable.** Freezing the interpreter and the APU together
  reaches 60.03 against a 60.15 cap — the whole emulation is worth +2.83 and the
  gap is +2.95. There is no missing lever; the sum of every lever is the budget.
- **Emulated fps is a dead instrument.** Everything saturates at 59.7–60.0.
  Judge on drawn fps (`g_snes_drawn_frames`), and read underruns with
  `stretch_ab.sh` — a wall-clock window gave 0/s and 9/s for the same build.
- **Every trade is at its optimum.** Forced draw stays at 1-in-4 (1-in-3 starts
  underrunning, 1-in-2 collapses); `SNES_ROMCACHE` stays off (+1.96 drawn but
  −1.22 emulated, and emulated fps is what feeds the audio). Only a change that
  raises emulated fps moves both axes.
- **Audio is decided.** Gap-free ships, by ear, and the ceiling makes it
  permanent rather than a stopgap.

**Shipped this session:** `SNES_PPU_OPAQUE_TILE` (+0.42 fps, hash-identical on
three ROMs across three rigs) and gap-free audio.

**Closed with numbers:** pair-store via memcpy (−4.7), the same branch without
the call (−0.8), 2bpp opaque path (0.00), sparse echo FIR (a savestate
refusal, not a win), WRAM before the ROM tag (−1.7 drawn), DSP idle-voice hoist
and skip (+0.16 / −0.59 drawn), `SystemClock_Config(3)` (dead code — the PLL
stays at level 2), spin learner at any setting (−4.78).

---

## Next session: what is actually unexplored (and what is not)

The wait-loop bake shipped and is verified (413/413 hash-identical, seven
cartridges on hardware, nothing regressing). Beyond it, two questions were
opened and neither was answered:

**Mario Kart and Super Metroid do not share a bottleneck with each other.**
Device profiles, play scenes, gates all PASS:

| | Kart | Metroid (interpreted) |
|---|---:|---:|
| 65816 interpreter | 28.0% | 42.3% |
| PPU | 25.0% | 23.3% |
| APU (SPC+DSP) | 19.1% | 12.9% |
| scheduler + DMA/HDMA | ~28% | ~21% |

**Do NOT re-open the PPU on the strength of that 25%.** The renderer ceiling is
already measured at +3.15 fps -- deleting the whole remaining render is worth
that and no more -- and the pixel loop, the SIMD pair, the coarse skip and the
sprite path have all been tried and lost. The share is large; the headroom is
not.

What is genuinely unmeasured:

- **HDMA, 226 calls a frame on Kart** (one per scanline; Mode 7 rewrites its
  perspective table every line). Nothing in this tree has ever priced it. An
  ablation knob was written and reverted, because it must run on the DEVICE
  play scene: the rig's cold-boot window sits on Kart's title screen, where
  there is no perspective table and the ablation changes nothing (the state
  hash was identical, which is how that was caught).
- **The native ports' own frames.** Super Metroid's port measures 56.2 fps on
  hardware WITH NO INTERPRETER AT ALL, and nobody has profiled it. Its frame is
  game C plus the PPU/APU emulation it shares byte-for-byte with this core, so
  whatever is left there is shared with every ported game. Note the interpreted
  Metroid numbers above are for a path no player uses -- Metroid, Mario World
  and Zelda 3 all have native ports, which is why they are the wrong ROMs to
  benchmark the SNES core with. Use Kart, Dragon's Magic, Amazing Tennis.
- `g_common_drawn_frames` / `g_common_emu_frames` (Core/Src/porting/common.c)
  make any core measurable with `drawn_ab.sh`, ports included -- verified live
  on the SNES core. Booting a native port unattended still does not work:
  `/snes_bench_index.txt` accepts a ROM NAME now, but the homebrew lookup does
  not launch Super Metroid and was not diagnosed.
