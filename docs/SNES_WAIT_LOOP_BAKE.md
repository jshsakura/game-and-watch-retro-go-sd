# Executing the SNES wait loop instead of interpreting it

A SNES game spends much of every frame waiting for its own NMI. The wait is four
bytes:

```
806b:  a5 10     LDA $10        ; direct page, low WRAM
806d:  f0 fc     BEQ $806b      ; back to itself
```

`SNES_SPIN_BAKE` finds those four bytes in the cartridge at load and executes
that loop directly, instead of dispatching two interpreter opcodes per
iteration. On a Game & Watch this is worth a fifth to two fifths of the frames
the player actually sees.

## What it is not

It is not the older spin-skip learner. That watched execution, proved a loop
pure over two laps, and then *replayed a recorded cycle pattern* — which is only
sound while something keeps proving it, and that proof cost 4.78 fps on
hardware, charged on every real opcode and repaid only on skipped ones.

It is also not "bake the learner's pattern into a table and replay it without
the learner". That was tried and it is unsound: with nothing keeping the pattern
honest, the loop cannot see the byte the NMI handler wrote, and the machine
diverges (`STATEHASH 74d314ee` against the correct `eb1a2262` — and *faster*,
which is what a wrong answer looks like).

What ships instead **executes the two opcodes**: the load reads the polled byte
out of WRAM and sets A/Z/N exactly as the interpreter would, and the branch is
taken on that Z. Nothing is assumed, so nothing has to be re-proved. The loop
leaves by itself on the first iteration after the handler writes the byte.

## Why it is address-agnostic

The match is on the loop's bytes, not on the cartridge. Super Mario World runs
it at `$00:806b` polling `$10`; A Link to the Past at `$00:8034` polling `$12`.
One recognizer covers both, every ROM hack of either, and every other cartridge
that waits the same way — with no per-ROM table to maintain and nothing to
update when a dump differs from the one someone measured.

The pc it computes is validated by reading it back through `cart_read()`, i.e.
through the emulator's own mapper. A mapping this scanner does not understand
therefore installs nothing rather than installing a pc where other code lives.

## Where the test lives, and why that is the whole design

The obvious shape — test each opcode's pc before dispatch — does not work on
this chip. Measured on hardware, a build that **installed nothing at all** still
lost 6.6% of A Link to the Past's drawn frames, and Super Mario Kart lost 10.5%.
`run_dots`' innermost loop is tight enough that adding any test to it costs more
than the test. Both shapes were tried (inlined, and behind a call).

Specialising the frame loop into `baked` and `plain` clones so the disarmed one
folds the test away is worse still: instantiating both changed how gcc allocated
the whole function, and the never-installing build lost **13.9%** of Zelda's
drawn frames.

A wait loop is entered once and spun thousands of times, so it does not have to
be noticed per opcode. `run_dots` asks **once per span**, and when the pc is in
the loop, `spin_bake_run_span()` replays laps until the loop breaks. Two details
carried most of the numbers, and both were found by measurement:

- **Spans usually start mid-opcode.** Returning when `cpuCyclesLeft != 0`
  replayed 33 laps a frame where the per-opcode version replayed 3,643.
- **The replay must run the DMA burst itself.** Handing the span back on DMA
  meant `run_dots` had to re-test the pc on every DMA cycle, which cost 1.1% of
  Super Metroid — a cartridge that contains no match at all. Running the burst
  inside the replay deletes the test rather than making it cheaper, and keeps
  the replay alive across A Link to the Past's per-scanline HDMA.

A per-frame gate disarms the loop (by moving the pc out of reach, not by
switching code paths) if it stops earning, and backs off by doubling while never
giving up: spin rate belongs to the *scene*, not the cartridge. The same ROM
measures 730 laps/frame in a boot window and 3,976 in its play scene.

## Hardware results

Drawn frames per second, on savestate scenes, bracketed A/B/A. Drawn frames and
not fps: three of these four scenes sit on the 60.15 fps audio-DMA cap, where
the overload guard converts spare time into drawn frames and the fps counter
cannot move. Emulated fps changes by less than 0.3 anywhere in this table.

| cartridge | off | on | |
|---|---:|---:|---|
| Super Mario World | 23.46 | **33.33** | **+42.1%** |
| A Link to the Past | 16.44 | **19.15** | **+16.5%** |
| Super Mario Kart | 16.95 | 17.25 | +1.8% |
| Super Metroid | 11.41 | 11.42 | 0.0% |

Nothing regresses. Super Metroid contains no match (`on=0, sites=0`) and pays
nothing measurable.

### A wider sample, on scenes the console made for itself

Seven cartridges, three flashes total: the ROM under test is a line in
`/snes_bench_index.txt` and the savestates are written by `GNW_AUTOSAVE_FRAME`
at frame 1200 of a cold boot. Those are early scenes — menus, attract, opening
play — not deep gameplay, and they are the same scene for both arms, which is
what an A/B needs.

| cartridge | off | on | | emulated |
|---|---:|---:|---|---:|
| A Link to the Past | 25.90 | **51.03** | **+97.0%** | 60.9 → 60.9 |
| Super Metroid (no match) | 11.71 | 11.70 | -0.1% | 46.8 → 46.8 |
| Super Mario World | 23.00 | **33.27** | **+44.7%** | 60.8 → 60.9 |
| Dragon's Magic | 25.30 | **34.17** | **+35.1%** | 53.6 → 54.2 |
| Super Mario Kart | 17.51 | **19.16** | **+9.4%** | 60.0 → 59.9 |
| SD Gundam Generation A | 61.01 | 61.10 | +0.1% | 61.0 → 61.1 |
| David Crane's Amazing Tennis | 35.10 | **60.68** | **+72.9%** | 61.0 → 60.9 |

David Crane's Amazing Tennis goes from dropping nearly half its frames to
drawing every one. Super Metroid, which contains no match, does not move. SD
Gundam already drew every frame and had nothing to gain — the mechanism cannot
help a cartridge that is not dropping frames, and does not hurt it either.

Measured with `tools/gnw_probe/drawn_ab.sh`, which resets the console and takes
a delta across a fixed number of emulated frames. A wall-clock window is not
the same window twice: one unchanged build read 20.73, 25.27, 21.70, 28.04,
24.40, 26.22 and 25.12 drawn fps across seven of them.

## Default

`SNES_SPIN_BAKE=1` is the shipping default. `=0` exists to measure what it is
worth, the way `SNES_SPC_IDLE_SKIP=0` does — it is not the state anything ships
in. A feature behind a default-off flag is a feature nobody receives, and this
repository has already shipped three releases with every flag at 0 for exactly
that reason.

Verified the way that failure would have been caught: a build with **no flags at
all** was flashed, and it resumes the savestate, installs the loop
(`00:8034/8036`, 5,031,141 laps) and draws 52.65 fps where the same scene without
it draws 25.90.

## Library-wide verification

The whole survey -- 2,075 cartridges scanned, 413 gated, the raw data and the
commands that regenerate it -- has its own page:
**[SNES_ROM_SURVEY.md](SNES_ROM_SURVEY.md)**. In short:

| | |
|---|---:|
| cartridges scanned | 2,075 |
| recognizer installs a loop | 413 (19.9%) |
| no match, costs nothing | 1,662 (80.1%) |
| gated hash-identical (state + audio, 300 frames, both arms) | **413 / 413** |
| loop observed firing in the cold-boot window | 106 |

The survey links the firmware's own `snes_loadRom()` and `spin_bake_scan()`, so
the mapper decision and the `cart_read()` validation are the ones that run on
the device -- a script that reimplemented either would be surveying a different
program.

`laps` counted in a cold-boot window says *whether* a loop is live, not how much
it is worth: A Link to the Past replays 730 laps/frame there and 3,976 in its
play scene.

## Closed: 32 kHz audio

The DSP already synthesizes all 534 samples of a frame -- 32 kHz -- and
`dsp_getSamples()` box-filters them down to 266 for a 16 kHz SAI, so everything
above 8 kHz is produced and then discarded. Raising the rate therefore costs no
synthesis, which made it look cheap.

It is not. On hardware, same savestate scene, drawn frames:

| | emulated | drawn |
|---|---:|---:|
| 16 kHz (ships) | 61.00 | **52.65** |
| 32 kHz | 61.00 | 21.2 |
| 32 kHz, stretcher constants scaled with the rate | 61.01 | 19.9 |

Emulated fps does not move -- samples per frame scale with the rate, so the
audio-DMA pacing is preserved -- and the drawn frames collapse by 62%. Twice the
SAI interrupts, and a stretcher doing per-sample autocorrelation over twice the
samples, cost far more than the wait-loop bake won. Scaling the stretcher's
constants (they are all written at 16 kHz) does not recover it, so the cost is
the rate itself and not a misconfiguration.

The stretcher constants now scale with `SNES_AUDIO_RATE` anyway, since measuring
a rate change against constants written for another rate is measuring the wrong
thing. At 16 kHz they are unchanged.
