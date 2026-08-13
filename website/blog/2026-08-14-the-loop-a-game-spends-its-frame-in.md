---
slug: the-loop-a-game-spends-its-frame-in
title: 'Four bytes, 413 cartridges: the loop a SNES game spends its frame in'
authors: [jshsakura]
tags: [snes, performance, testing, hardware]
image: /img/probe-rig-running.jpg
---

A SNES game waits for its own NMI, and the wait is four bytes:

```asm
806b:  a5 10     LDA $10        ; direct page, low WRAM
806d:  f0 fc     BEQ $806b      ; back to itself
```

The emulator dispatches those two opcodes, thousands of times a frame, to
discover each time that the byte has not changed yet. Executing that loop
directly instead — same reads, same flags, no dispatch — is worth **+97% of the
frames a player actually sees** in A Link to the Past.

The interesting part is not the trick. It is that the frame counter cannot see
it, that the obvious way to implement it makes cartridges *slower*, and that
proving it safe meant running 413 cartridges rather than the two on my desk.

{/* truncate */}

## The counter still could not see it

[Last time](/devlog/the-frame-counter-was-lying) the lesson was that fps is
paced by the audio DMA, so a real saving can read as nothing. This time the
counter is even less use, because it counts the wrong frames.

The overload guard draws roughly **one emulated frame in four**. So the number
that moves when rendering gets cheaper is not fps — the guard converts the
slack into drawn frames and fps stays put. In the table below, emulated fps
changes by less than 0.3 anywhere, while what the player sees doubles.

| cartridge | drawn fps, off | drawn fps, on | |
|---|---:|---:|---|
| A Link to the Past | 25.90 | **51.03** | **+97.0%** |
| David Crane's Amazing Tennis | 35.10 | **60.68** | **+72.9%** |
| Super Mario World | 23.00 | **33.27** | **+44.7%** |
| Dragon's Magic | 25.30 | **34.17** | **+35.1%** |
| Super Mario Kart | 17.51 | **19.16** | **+9.4%** |
| SD Gundam Generation A | 61.01 | 61.10 | +0.1% |
| Super Metroid | 11.71 | 11.70 | −0.1% |

The last two rows are the ones that make this a measurement instead of an
advertisement. SD Gundam was already drawing every frame and had nothing to
win; Super Metroid contains no matching loop at all. Neither moves — which is
exactly what a mechanism that only removes waiting should do.

## Where the test lives is the whole design

The obvious implementation tests each opcode's address before dispatch. On this
chip that loses, and it loses on cartridges where it never fires once:

| where the test lives | cost on a cartridge with nothing installed |
|---|---|
| per opcode | −6.6% drawn (Zelda), −10.5% (Mario Kart) |
| two specialised frame-loop clones | −13.9% drawn (Zelda) |
| **once per span** | 0.0% (Super Metroid) |

The inner loop is tight enough that adding *any* test costs more than the test
can repay. And the clever fix — compile two copies of the frame loop so the
disarmed copy folds the test away — is worse than the disease: instantiating
both changed how gcc allocated registers across the *whole* function, and the
build that installed nothing at all lost 13.9%.

A wait loop is entered once and spun thousands of times. It does not need to be
noticed per opcode. Asking once per scheduler span costs nothing measurable and
catches all of it.

## Two cartridges is not evidence

The recognizer matches bytes, not cartridges — Super Mario World waits at
`$00:806b` polling `$10`, A Link to the Past at `$00:8034` polling `$12`, one
rule covers both plus every ROM hack of either. That is a nice property and also
a claim about a whole library, made on the strength of the two ROMs I happened
to test.

So the library got tested.

| | |
|---|---:|
| cartridges scanned | **2,075** |
| recognizer installs a loop | 413 (19.9%) |
| no match, costs nothing | 1,662 (80.1%) |
| **gated hash-identical** (state + audio, both arms, 300 frames) | **413 / 413** |

Two rules made that worth reading. **The survey links the firmware's own
`snes_loadRom()` and `spin_bake_scan()`** — the mapper choice is a score across
four candidate header positions, so a Python reimplementation would have
surveyed a different program. And **the gate runs the Thumb-2 engine the device
runs**, not a host interpreter, because the cycles being replayed are that
engine's.

24 seconds for the survey, a few hours for the gate, and the answer to "does
this change any game's emulation" stopped being an opinion.

## What it does not prove

`laps` — how often the loop actually fires — was counted in a 300-frame window
from cold boot, and a cold boot is logos and title screens. A Link to the Past
replays 730 laps/frame there and **3,976** in its play scene, a factor of 5.4.
So 106 of 413 firing in that window says *whether* the loop is live, not how
much any of them is worth. The seven cartridges in the first table are the ones
measured where a game is actually played, on savestate scenes the console wrote
for itself with `GNW_AUTOSAVE_FRAME` — nobody can play a console with a debug
probe soldered to it.

## And what it replaced

There was an older, cleverer version of this: a learner that watched execution,
proved a loop pure over two laps, and replayed a recorded cycle pattern. It cost
**−4.78 fps on hardware** — and the per-ROM whitelist had already said "no" for
the cartridge being measured. A table that answers "no" does not stop the tax,
because the tax is the discovery machinery, not the decision.

Baking that recorded pattern into a table and replaying it without the learner
is worse than slow: with nothing keeping the pattern honest the loop cannot see
the byte the NMI handler wrote, and the machine diverges — `STATEHASH 74d314ee`
against the correct `eb1a2262`, and *faster*, which is what a wrong answer looks
like.

What ships **executes the two opcodes**. Nothing is assumed, so nothing has to
be re-proved.

## Written down, so it is not re-derived

Alongside this, the whole SNES ledger was rewritten: every road driven to its
end, each with the number that closed it, and — the part that matters more —
what is *not* closed. The measured ceiling says the 65816 and the APU together
are worth +2.83 fps against a +2.95 gap to the audio cap, so "find one more
lever and we reach 60" is arithmetic that does not work. The remaining honest
targets are HDMA (226 calls a frame on Mario Kart, never priced) and the native
ports' own frames.

- [The library measurement, with the raw data](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SNES_ROM_SURVEY.md)
- [The feature write-up](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SNES_WAIT_LOOP_BAKE.md)
- [The optimization ledger](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/OPTIMIZATION_LEDGER.md)
