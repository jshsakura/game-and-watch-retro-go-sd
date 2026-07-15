---
slug: video-slowdown-two-clocks
title: "The video player's slowdown was two clocks quietly fighting"
authors: [jshsakura]
tags: [video, performance, audio]
---

The video player got progressively slower the longer a clip ran — fine at first, choppy after a
few minutes, worse still on a long clip, and it never recovered until you seeked. "The SD card is
slow" is the obvious guess, and it's the wrong one: a bandwidth ceiling makes playback *uniformly*
slow, not *progressively* slower. Progressive means something is **accumulating**.

{/* truncate */}

## What accumulates

The demuxer turned out to be O(1) per frame — nothing in the hot path grows with file position.
The only thing that grows is the **audio ring buffer's fill level**, and it grows because the
player runs on two clocks that don't agree:

- **Video** paces frames on the SysTick millisecond timer.
- **Audio** is drained by the SAI interrupt at the audio PLL's real 48 kHz.

Different oscillators, different dividers; the error only accumulates in one direction.
`trim_step()` is meant to hold the ring level steady by nudging the resampler ±1% — but it was a
*pure proportional* controller, and a proportional controller needs a standing error to produce a
standing correction. So under real drift the ring settled **above** its target. Push the mismatch
to the ±1% authority limit and the ring climbs without bound, fills up, and — the nasty part — a
full ring holds the video prefetcher's gate shut. Every later frame becomes a blocking read. That
is the cliff, and a seek is the only thing that resets it.

## Proving it before fixing it

I've been running everything through a QEMU Cortex-M7 rig lately (the same approach behind the
recent WonderSwan and Virtual Boy work), so I built one for video: it boots the **real** player
code on an emulated M7 with two deliberately-mismatched clocks and a synthetic clip, and prints a
per-frame ledger. It reproduced the bug exactly — and pinned the threshold at **precisely 1%**,
which is exactly the servo's authority. The latch happened at the same *wall-clock time* regardless
of clip length: proof it is time-dependent, not position-dependent. "SD is slow" was a red herring.

## The fix

Two parts:

1. **Proportional → PI.** An integral term drives the steady-state error to zero, so the ring
   converges *on* its target for any mismatch within authority — with anti-windup so the integrator
   can't run away.
2. **A non-latching valve.** If the ring ever fills past a cap despite the trim, drop the oldest
   audio sample. This *guarantees* the ring can never pin full and latch the gate, even if the real
   mismatch exceeds ±1%. Inside authority it never fires; beyond it, it sheds exactly the excess — a
   tiny periodic audio blip instead of permanent stutter.

Same rig, after the fix: bounded ring, no latch, prefetcher alive, at both 1% and 2% mismatch.
Device verification still to come — the rig models the logic faithfully but not absolute timing —
but the mechanism is nailed.

It's an experimental fork, so: rough, and I'm sure there's more to find. But this one felt good to
catch.
