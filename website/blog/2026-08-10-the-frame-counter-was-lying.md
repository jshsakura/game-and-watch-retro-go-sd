---
slug: the-frame-counter-was-lying
title: 'The frame counter was lying, and half a day went to the wrong scene'
authors: [jshsakura]
tags: [snes, performance, hardware]
---

An optimisation removes 5.5% of the instructions the emulator executes. It is
bit-identical — same state hash, same audio hash, same framebuffer. You flash it
and the frame counter moves from 57.28 to 57.40.

Two tenths of a frame. For 5.5%.

{/* truncate */}

That number is where this day started, and almost everything useful that came out
of it came from refusing to accept it.

## The counter counts audio periods, not speed

The SNES loop waits for one audio-DMA tick every frame. The tick comes every
16.625 ms — 60.15 Hz, one half-buffer of 266 samples at 16 kHz. So a frame that
finishes early sleeps out the rest of its period, and a frame that overruns costs
the next one too. Written out:

```
paced fps = 1 / E[max(frame_work, T)],   T = 16.625 ms
```

The mean has almost nothing to do with it. What decides the rate is how many
frames land past the line.

To see the raw rate you have to take the wait away, so `SNES_PACE_OFF=1` now
does that — diagnostic only, the audio free-runs. Same build, same scene:

| | fps |
|---|---|
| paced | 57.28 → 57.40 |
| **uncapped** | **58.41 → 59.53** |

The optimisation was worth **+1.9%**. The counter showed +0.2% because the
counter was measuring the audio hardware.

## Then a histogram, because the formula needs a distribution

One DWT read per frame, bucketed into RAM, read out over SWD while the game runs
(`SNES_FRAME_HIST=1`, `tools/gnw_probe/frame_hist.py`). No SD writes — writing to
the card during play is how you corrupt a FAT.

Feed the measured distribution into the formula and it returns **57.4**. The
measurement was 57.40. The model is exact, which meant the target was now a
number rather than a feeling: 23% of frames crossed the line.

## And then the scene turned out to be wrong

All of that was measured where the device boots with nobody at the console — the
title screen and its attract demo. The user made a savestate and asked why we
weren't measuring where the game is actually played.

Real gameplay is a different machine.

| | frames | emulation | total |
|---|---|---|---|
| under the period line | 17,601 | 12.87 ms | **14.76 ms** |
| over the period line | 5,884 | 30.52 ms | **33.99 ms** |

Twenty frames out of 23,400 fall in between. A frame is 14.6 ms or it is 32.4 ms.
And the thing that decides is not scrolling, which was the whole premise of the
morning — it is whether the frame is **drawn**. Two of the 5,884 slow frames were
frameskipped. The render costs 17.65 ms, an entire audio period, and the overload
guard was drawing one frame in four.

It also explains the stutter the user could hear in the rain, exactly. A frame
produces one period's worth of samples; a drawn frame spans two. At 51 fps the
core makes **14% fewer samples than the DMA consumes**, and the stretcher's
playback band is ±1%. That sound does not exist. No audio code can make it.

## The rule that came out of it

Eleven candidates were measured on hardware. Four won. The four that won all
**deleted work**. Of the seven that lost, most **added a test to skip work**:

| | fps |
|---|---|
| colour-math compositing, two pixels per iteration | 50.39 → 47.99 (**−4.8%**) |
| DSP idle-voice BRR skip | 50.39 → 48.85 (**−3.1%**) |
| bus charged once per opcode instead of per access | 50.39 → **51.51** |
| colour-math table padded to 8 rows, per-pixel range test deleted | 51.51 → **51.92** |
| spin learner's replay branch compiled out | 51.92 → **52.36** |

The reason is that the core is **stall-bound, not instruction-bound** — that is
what −5.5% instructions buying +1.9% actually told us, back at the top. A test in
an inner loop is paid on every iteration that fails it, and in the scenes that
are slow, most iterations do fail: in a translucent scene most pixel pairs do not
qualify; in a scene with music most DSP voices are not idle.

So: *does this remove work, or does it test for work to skip?* Expect the second
kind to lose.

With one caveat that cost 0.64 fps to learn. It does not mean "delete every
check". Two DSP idle fast paths skip a whole BRR decode and a Gaussian
interpolation — real work, not a handful of instructions — and deleting those
lost immediately.

## Three ways the measurements were quietly wrong

This is the part worth reading twice, because none of these announced themselves.

**The build system was not rebuilding.** `FLAGS_STAMP` recorded `C_DEFS` and
`CFLAGS`, while the SNES compile recipe adds eight define groups of its own.
Toggling a SNES knob rebuilt nothing — so the two arms of an A/B were
byte-identical binaries, and the comparison measured the same build twice. That
is worse than a wrong answer, because it looks like a real one. Adding the groups
to the stamp was not enough either: it is a `$(shell)`, it runs where it is
parsed, and it sat hundreds of lines above where those variables are assigned.
It had to move to the bottom of the file.

Now every A/B ends with `cmp` on the two `snes.bin` files before anyone believes
a number.

**The benchmark stopped measuring when the scene got better.** Booting from a
savestate restores `snes->frames` too, so the benchmark's "wait for the counter
to drop below 100" — its proof that the reset had happened — fell straight
through. It reported 700 fps. Then 2,675 fps. It now takes a base reading once
the ROM is alive and times a delta.

**The benchmark bricked the device.** A diagnostic build hardfaulted. The
benchmark, unable to tell "slow" from "does not boot", reset and polled, reset
and polled — while the firmware's own anti-brick counter watched each boot fail.
Three consecutive failures park the device at the rescue screen, which powers
itself off after a minute. Recovering it needed the rescue screen's `A` press
injected over SWD, by breaking on `buttons_get` and returning the mask by hand.

A measuring tool that cannot distinguish a slow build from a dead one will
eventually destroy the thing it measures. Both of its waits are bounded now, and
it says which failure it was instead of trying again.

## Two things a second reader caught

A research pass over the renderer corrected the brief it had been given:

- The colour-math lookup table is **6 KB, not 16 KB**. The theory that it was
  thrashing a 16 KB D-cache was wrong, and acting on it — growing the table —
  would have made things worse.
- `snes_frame` is a **persistent 158,720-byte full-frame buffer**, not the
  one-line render buffer an earlier analysis document describes. Two of the five
  reasons that document gives for closing a particular lever are therefore stale.
  The other three still hold, so the lever stays closed — but the grounds needed
  saying.

Two of the four shipped changes came out of that same pass: the table rebuild was
computing 3,072 entries where only four rows are ever distinct, and the layer
nibble of a z-word never exceeds 6, so padding the table to a power of two lets
both compositing loops drop a per-pixel range test.

## And one that measured neutral, for a reason worth keeping

The sprite pixel loop does a read-modify-write per pixel so the first writer
wins. Draw the accepted slivers in reverse and store unconditionally and you get
the identical buffer with the load gone — the renderer's own TODO asks for it,
and it is what the hardware does. Implemented, gated on two ROMs, hashes
identical on both. Device: **51.94 against 51.92**. Nothing.

The first instinct was to blame the scene: not enough sprites. So we counted —
5.46 slivers per line against a limit of 34, the limits never once reached. Light,
yes. But that is not why it was neutral. The change removes one load per sprite
*pixel*, eight per sliver, and adds about four stores per sliver to record it.
8:4 holds at any sliver count. A scene at the limit scales both sides equally and
lands in the same place.

Structurally neutral, not scene-dependent. Reverted, and closed with a ratio
instead of a guess.

## What it adds up to

**50.39 → 52.36 fps in real gameplay**, +3.9%, everything bit-identical on the
rig. The frame is now roughly three equal thirds — PPU 23%, APU 22%, CPU 24% —
with no dominant item left, which is its own kind of answer.

Sixty frames in that scene would need the render at 7.4 ms instead of 17.65. Not
one of the eleven candidates comes close. Lighter scenes already sit at 57–59.

The tools are the durable part: an arm builder that remembers the SNES core lives
on the SD card and pushes it too, a frame histogram, a pacing-off switch, a way
to stop the compiler folding three render functions into one 5.6 KB symbol the
profiler could only report as a single number.

And the rule. Five times in a row it predicted the device correctly. The one time
it did not, it was because it had been passed on as "delete every check" instead
of what it says.
