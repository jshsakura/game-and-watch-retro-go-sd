# SNES: execute the NMI wait loop instead of interpreting it (+35–97% drawn frames, 413 cartridges gated)

A SNES game spends much of every frame waiting for its own NMI, and the wait is
four bytes:

```
806b:  a5 10     LDA $10        ; direct page, low WRAM
806d:  f0 fc     BEQ $806b      ; back to itself
```

`SNES_SPIN_BAKE` finds those bytes in the cartridge at load and **executes that
loop directly** instead of dispatching two interpreter opcodes per iteration.
The match is on the loop's bytes, not on the cartridge, so it needs no per-ROM
table: Super Mario World runs it at `$00:806b` polling `$10`, A Link to the Past
at `$00:8034` polling `$12`, and one recognizer covers both plus every ROM hack
of either.

## Hardware results

Drawn frames per second, on savestate scenes the console wrote for itself, both
arms on the same scene. Drawn frames and not fps: most of these sit on the
60.15 fps audio-DMA cap, where the overload guard turns spare time into drawn
frames and the fps counter cannot move (emulated fps changes by <0.3 anywhere
in this table).

| cartridge | off | on | | emulated |
|---|---:|---:|---|---:|
| A Link to the Past | 25.90 | **51.03** | **+97.0%** | 60.9 → 60.9 |
| David Crane's Amazing Tennis | 35.10 | **60.68** | **+72.9%** | 61.0 → 60.9 |
| Super Mario World | 23.00 | **33.27** | **+44.7%** | 60.8 → 60.9 |
| Dragon's Magic | 25.30 | **34.17** | **+35.1%** | 53.6 → 54.2 |
| Super Mario Kart | 17.51 | **19.16** | **+9.4%** | 60.0 → 59.9 |
| SD Gundam Generation A | 61.01 | 61.10 | +0.1% | already full rate |
| Super Metroid | 11.71 | 11.70 | −0.1% | no match in ROM |

Amazing Tennis stops dropping frames altogether. A cartridge with nothing to win
(SD Gundam, already drawing every frame) loses nothing, and one the recognizer
does not match (Super Metroid) does not move.

## Verification

- **Library survey, 2,075 cartridges, 24 s.** 413 get a loop installed (19.9%);
  1,662 match nothing and cost nothing. The survey links the firmware's own
  `snes_loadRom()` and `spin_bake_scan()` — the mapper decision is a score
  across four candidate header positions, so a script that reimplemented it
  would be surveying a different program.
- **Hash gate on all 413.** Both arms of the M7 QEMU rig (the Thumb-2 engine the
  device runs), 300 frames each, comparing state and audio hashes with the
  feature off and on: **413/413 identical, zero divergences.** 106 of them
  replayed at least one lap inside that cold-boot window.
- The installed pc is validated by reading it back through `cart_read()`, i.e.
  through the emulator's own mapper, so a mapping the scanner does not
  understand installs nothing rather than installing a pc where other code
  lives.

## What it deliberately is not

It is not the older spin-skip learner, which watched execution, proved a loop
pure over two laps and replayed a recorded cycle pattern. That proof cost 4.78
fps on hardware — charged on every real opcode, repaid only on skipped ones.

It is also not "bake the learner's pattern into a table and replay it". With
nothing keeping the pattern honest, the loop cannot see the byte the NMI handler
wrote and the machine diverges: `STATEHASH 74d314ee` against the correct
`eb1a2262`, and *faster*, which is what a wrong answer looks like.

What ships **executes** the two opcodes — the load reads the polled byte from
WRAM and sets A/Z/N exactly as the interpreter would, the branch is taken on
that Z — so nothing is assumed and nothing has to be re-proved.

## The one design decision worth reviewing

The test is **once per `run_dots` span**, not per opcode, and that is the whole
difference between shipping this and not:

| where the test lives | cartridge with nothing installed |
|---|---|
| per opcode | −6.6% drawn (Zelda), −10.5% (Mario Kart) |
| two specialised frame-loop clones | −13.9% drawn (Zelda) |
| **once per span** | 0.0% (Super Metroid) |

`run_dots`' innermost loop is tight enough that adding *any* test to it costs
more than the test; specialising the loop so the disarmed clone folds the test
away is worse still, because instantiating both clones changes how gcc allocates
the whole function. A wait loop is entered once and spun thousands of times, so
it does not have to be noticed per opcode.

Two details inside the span replay carried most of the numbers, both found by
measurement: spans usually start mid-opcode (returning on `cpuCyclesLeft != 0`
replayed 33 laps a frame where the per-opcode version replayed 3,643), and the
replay must run the DMA burst itself (handing the span back meant re-testing the
pc on every DMA cycle, which cost 1.1% of a cartridge that contains no match).

A per-frame gate disarms the loop if it stops earning and backs off by doubling
while never giving up — spin rate belongs to the *scene*, not the cartridge: the
same ROM measures 730 laps/frame in a boot window and 3,976 in its play scene.

## Flag and default

`SNES_SPIN_BAKE=0` by default; nothing changes unless it is turned on.

Full write-up, including the measurement tooling and how to reproduce any of the
numbers: `docs/SNES_WAIT_LOOP_BAKE.md`.
