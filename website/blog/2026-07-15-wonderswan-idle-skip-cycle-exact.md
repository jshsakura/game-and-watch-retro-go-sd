---
slug: wonderswan-idle-skip-cycle-exact
title: "WonderSwan at 75 fps on the stock clock: an idle-skip that had to learn what 'exact' means"
authors: [jshsakura]
tags: [wonderswan, performance]
---

The last WonderSwan post ended with the renderer a third lighter and the heaviest battle frame
just barely inside the stock CPU budget. Good — but "just barely" is not how a 75 fps handheld
should live, and the overclock was still on as insurance. This is the story of removing the other
big cost, getting it wrong in a way that was almost invisible, and being told — correctly — that
almost doesn't count.

{/* truncate */}

## Most of the CPU time is the game doing nothing

Profile the guest instruction stream of **One Piece Grand Battle** and about **83% of executed
instructions are one tiny loop**: compare a memory flag, jump back if it hasn't changed. The game
is waiting for the video interrupt to end the frame. The emulator dutifully re-executes that
comparison thousands of times a frame, computing the same answer.

The detector for this is careful by construction. On every taken backward branch, hash the
complete register file, the segment registers and the flags. If an iteration comes back to the
same address with the same hash, **wrote no memory** and **touched no IO**, it provably cannot
make progress — its only exit is an interrupt. So stop running the CPU. Let the hardware advance
to the interrupt, deliver it, and wake the CPU. A delay loop that counts (`DEC CX`) changes the
hash; a loop that polls an IO port sets the IO flag; neither is ever skipped.

CPU emulation dropped **77%**. The whole frame — CPU, renderer, blit — fell from 4.25M
instructions to **2.30M, or 61% of the stock 280MHz budget**. One Piece came out bit-exact across
2000 frames. Ship it?

## 27 games say no

Sweeping the full 93-ROM library said: 65 games bit-identical, and **27 games with a one-frame,
one-scanline, six-pixel flicker** somewhere in a 20-second run. Invisible in practice — I measured
one of them at 6 changed pixels out of 32,256 for a 75th of a second. I called it imperceptible
and shipped it.

The review from the project owner was short and right: *a 75 fps machine doesn't get treated like
that.* The glitch wasn't noise to be excused. It was a symptom to be explained.

## State-exact is not cycle-exact

The explanation is the most useful thing in this post. The skip preserved every byte of machine
*state*, but not the machine's *clock*:

- Each CPU slice returns how many cycles it actually consumed, and the scheduler carries the
  remainder into the next slice. A parked CPU returned nothing — **the carry froze**.
- An interrupt is really delivered at whatever mid-loop instruction boundary the slice budget ran
  out on. The parked version delivered it **at the top of the loop** — different stacked return
  address, and the delivery check reads the interrupt-enable flag *at that boundary*.

Neither changes what the game computes. Both change *when* mid-frame writes land relative to the
raster — by about one scanline. Hence: rare single-scanline flickers, only in games that write
video registers mid-frame.

The fix makes parking reproduce an unskipped run boundary for boundary. Before parking, one loop
iteration's **per-instruction cycle pattern** is recorded. A parked slice then replays the
interpreter's cycle arithmetic over that pattern — pure arithmetic, no instruction dispatch, which
is the whole saving. On wake, the CPU **really executes** the partial iteration from where it
stopped to the boundary the spin would have reached (the loop is state-invariant, so this is free
of side effects), and only then is the interrupt delivered — same registers, same flag state, same
stacked address, same carry.

And because a claim like that needs a policeman: there is a **self-verifying build** where every
parked slice *also* runs the real interpreter and asserts the prediction matches, cycle for cycle.
Sabotage the arithmetic by one cycle and it aborts on the first parked slice.

## Gates, then hardware

The pass bar was raised to match the lesson: not "looks the same", but the **whole framebuffer
plus all 64KB of guest RAM, hashed every frame**, idle-skip on versus off.

- Local library: **93 of 93 bit-identical**.
- A second library on another machine (209 ROMs including Color): **206 of 206 that run,
  bit-identical**. The three exceptions are known-bad dumps that hang identically in the
  *unmodified* core.
- The strict gate initially cried wolf on three games — and the culprit was an upstream renderer
  bug worth having found: a stack buffer whose padding bytes were never initialized, gating
  margin pixels that no screen ever shows. Garbage in, nondeterminism out; a `memset` ended it.

Then the part no rig can do: the device. Save, load, cold resume, heavy battles — **a smooth
75 fps on hardware**. With the heaviest frame at 61% of the stock budget and the remaining margin
never touched, the scoped overclock the port shipped with has nothing left to insure.
WonderSwan now runs on the **stock 280MHz clock**.

## What this cost, honestly

The first shipped version of the idle-skip was withdrawn and superseded within hours. The final
version is faster to a rounding error, provably exact instead of approximately exact, and it
found an upstream bug on the way. The difference between the two was one sentence of review:
if the machine runs at 75 frames a second, then all 75 belong to the game.
