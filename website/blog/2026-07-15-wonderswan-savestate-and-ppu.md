---
slug: wonderswan-savestate-and-ppu
title: "WonderSwan: the resume bug the web version hid, and a third off the renderer"
authors: [jshsakura]
tags: [wonderswan, savestate, performance]
---

Two WonderSwan problems had been sitting on the "someday, maybe" pile for a long time: one game
that refused to resume from a savestate, and a renderer that dropped frames in busy scenes. This
week both got solved — and the fix for the first one came out of a single offhand hint.

{/* truncate */}

## First, a way to see what the device actually runs

Before touching anything I built the same rig the GBA work used: the WonderSwan core compiled from
its **own device source list**, with the device's defines, run as a real ARMv7‑M instruction stream
under QEMU. That turns "it feels slow" into a number — instructions per frame, split into CPU
emulation, the PPU (the per‑scanline renderer), and the final blit to the screen.

Pointed at **One Piece Grand Battle**, the busiest thing in the library, the answer was blunt: in a
fight the **renderer**, not the CPU, was the heavy part — about 2.2M instructions a frame, more than
the emulated CPU itself, pushing the whole frame past even the overclocked budget. That is exactly
what a dropped frame looks like.

## The renderer, minus a third

The background and foreground layers run the same tile loop, and it was doing two wasteful things
per tile: re‑deriving eight pixel indices from the raw bitplanes with a column of shifts and masks,
and re‑testing the tile's transparency flag and re‑indexing the palette on every one of the eight
writes. Both are constant work that can be lifted out — the bit‑spread became a small precomputed
table, and the per‑tile decisions moved above the pixel loop.

The result: **PPU cost down ~32%**, and the worst battle frame dropped from **4.27M to 3.64M**
instructions — back under the *stock* 280 MHz budget, not just the overclocked one. And because the
rig prints the same frame hashes the core produces, I could prove the output was **byte‑for‑byte
identical** across 2000 frames including gameplay. No visible change, just fewer instructions. (The
mild level‑1 overclock stays, as headroom for the cache and flash‑XIP stalls the rig can't see.)

## The savestate that only worked on the web

The other problem was stranger. One Piece Grand Battle would **freeze the moment you loaded a
savestate** — while every other game resumed fine. It had defeated an earlier attempt, and it very
nearly defeated this one.

The hint that cracked it was one line: *"it works on the web version."*

That shouldn't be possible if the save file is incomplete — unless the web version never does what
the device does. And it doesn't. The web front‑end loads a savestate **into the emulator that's
already been running** since you opened the page — a *warm* load. The Game & Watch loads into a
**freshly reset** core — a *cold* load. So I built a harness that splits the two: it records a save
in one process, then resumes it in a **brand‑new process**, which is what a device cold boot really
is. On the host that finally reproduced the freeze that only ever happened on hardware.

From there it was detective work. Everything the save is supposed to carry matched perfectly at the
load boundary — memory, registers, bank mapping, even the sound and timer accumulators I'd added
along the way. Yet the CPU walked off a cliff a few thousand instructions into the first frame. So I
logged **every instruction** of that frame in both the working and broken runs and diffed them. They
were identical for 12,679 instructions, then split on a single conditional branch. Disassembling the
ROM there:

```
CMP word [110c:0046], 0
JZ  ...
```

It was reading a byte of **scratch RAM** — and the good run saw the value the game had written,
while the cold run saw `0xA0` filler. The game parks a memory segment on the emulator's shared
"unmapped bank" and uses it as RAM, and **that scratch region was never part of the savestate**. The
web works because a warm machine still has the game's writes sitting there; a cold boot wipes them.

Saving that region (as a backward‑compatible trailer, so old saves still load) fixed it. The
two‑process cold‑boot harness now resumes One Piece with **zero game‑state divergence** across
hundreds of frames and several save points — and Final Fantasy, which already worked, still passes.

## Honest status

- Fixes are proven on the host harness (instruction‑exact) and the firmware builds clean; the final
  word, as always, is the device.
- The renderer change is output‑identical by construction, so it carries no visual risk.
- A single post‑load frame differs cosmetically (the sprite table refills one frame later); the game
  state itself is exact.

Two off the "someday" pile. The tooling — a device‑accurate instruction rig and a cold‑boot resume
harness — is the part I'll reuse the most.
