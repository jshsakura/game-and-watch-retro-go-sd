---
slug: the-app-that-never-asked-for-the-clock
title: 'The app that never asked for the clock'
authors: [jshsakura]
tags: [video, performance]
image: /img/clock-hero.jpg
---

Every heavy emulator in this firmware asks for an overclock at startup. GBA, SNES,
Virtual Boy, WonderSwan — one line each, and the launcher lifts the core from
280 MHz to 340.

The video player did not. It is the one app that, inside every single frame, does a
blocking SD read, a hardware JPEG decode, an MP3 decode, a resample, and a
full-screen blit. It had been running at stock the whole time.

{/* truncate */}

## Why nobody noticed

Because the player had been *fixed* four times, and each fix was correct.

The SD read was the bottleneck, so the read got faster — the FatFs driver was
reading each 512-byte block one byte at a time, 512 SPI calls per block, capping
throughput near 243 KB/s no matter what the SPI clock was. One block transfer per
block, and per-frame read time fell from ~32 ms to single digits.

The JPEG decoder refused every frame, so the decoder got fixed — a lock in the HAL
handle that `HAL_JPEG_Init()` looks like it resets and does not.

Playback degraded after four minutes, so the A/V clocks got a servo — a PI trim with
a non-latching valve, proven against a QEMU Cortex-M7 rig with two independent
clocks.

Four rounds, each one a real fault with a real fix. And every one of them was about
making the *work* smaller or the *timing* correct. Nobody asked whether the machine
was running as fast as it could while doing it, because the question sits in a
different category from all four.

## The one-liner

```c
common_emu_auto_oc(2);      // 340 MHz, the top of the launcher's own scale
```

It is worth more here than the raw +21% suggests. The SD read is not a DMA
transfer — it is a CPU-driven SPI loop. The clock speeds up the bytes, not just the
arithmetic around them.

Level 2 and not the core-private level 3 (353 MHz), deliberately: a clip is
sustained load for ten minutes, and 353 is exactly the level that proved unstable
under sustained load elsewhere in this tree. `common_emu_auto_oc()` is a floor
rather than a setting — it skips the boost entirely on one SD hardware variant that
crashes when overclocked, and a user who chose a higher level keeps it.

## What it unlocks

The encoder had been pinned at 20 fps, and the reason was written down:

> fps=20 — fewer frames/s = fewer SD reads. per-read latency is the bottleneck, so
> fps↓ (read count) + q↑ (sectors/read) both cut SD load directly.

Every word of that was true when it was written. None of it is true now: the
per-read latency it names is the byte-at-a-time loop that was replaced. **A comment
that documents a constraint outlives the constraint**, and then it reads like a law.

30 fps at the same per-frame quality is about 300 KB/s against a path that now does
megabytes. The frame-rate change came with a matching raise to the VBV ceiling,
because leaving it would have made rate control raise the quantiser on ordinary
scenes — buying smoothness by spending sharpness, which is not an improvement.

## The cliff nobody was counting

While in there, one more thing that had been invisible: a frame larger than the
64 KB decode slot cannot be read at all, so it is queued as a failure marker and
never drawn. That is the correct behaviour — there is nowhere to put it — and on
screen it is indistinguishable from SD judder or a slow decode.

Nothing counted it. The encoder keeps peaks under the ceiling with rate control, so
it probably never happens; "probably" is doing a lot of work in that sentence, and
it costs two counters to replace it with a reading. The debug HUD now shows the
largest frame the clip contains and how many did not fit, so a clip running *close*
to the ceiling is visible before it starts crossing it.

That is the difference between a limit you chose and a limit you inherited. Both
were 64 KB; only one of them was a decision.

## And resume, because it was one line of feature and three lines of judgement

Reopening a clip started it over. It picks up where it stopped now — a line per clip
in a small text file, read on open, written once playback has already stopped
(the player must not touch the SD while it is decoding).

The feature is trivial. What makes it feel right is three edges, and each is a test
case rather than an opinion:

- a position in the first ten seconds is ignored — resuming four seconds in is worse
  than starting over
- a position within five seconds of the end **erases** the entry, or "continue"
  drops you in the credits, which looks exactly like the clip refusing to play
- rewriting one clip's position must not lose the others'

The host test earned its keep immediately: finishing the *only* clip in the store
dropped its entry in memory and then returned without writing, so the old file
stayed on disk and the position that was supposed to be erased came back on the next
open.

And the linker earned its keep too. The obvious implementation collects the
surviving lines in a `static char[32][266]` and rewrites — 8.5 KB of BSS, which this
overlay does not have. `Error: MUSIC BSS overflow`, refused to link. Streaming
through a temp file and committing with a rename costs no BSS at all, and is
crash-safe as a bonus: the live file is only replaced once the new one is complete.
