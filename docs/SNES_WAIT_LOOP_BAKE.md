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

## Reproducing

```sh
# what the recognizer would install, across a library, using the firmware's
# own loader and scan
bash tools/snes_bake_survey/run.sh /path/to/roms survey.tsv

# both arms of the M7 rig over every cartridge it installs into, comparing
# state and audio hashes
python3 tools/snes_bake_survey/hash_gate.py /path/to/roms \
        --only-installs survey.tsv --out gate.csv

python3 tools/snes_bake_survey/report.py survey.tsv gate.csv

# on hardware: three flashes, one line per cartridge
bash tools/gnw_probe/bake_sweep.sh indices.txt
```

The device pipeline needs a scene it can resume. `GNW_AUTOSAVE_FRAME=n` has the
console boot a ROM, run n deterministic frames and write savestate slot 0 once —
nobody can play a console with a debug probe soldered to it. The ROM under test
comes from `/snes_bench_index.txt` on the card, so a sweep is two builds and a
reset per cartridge rather than two container builds per cartridge.

## Library-wide verification

## Library survey

`tools/snes_bake_survey/run.sh` links the firmware's own `snes_loadRom()` and
`spin_bake_scan()`, so the mapper decision and the cart_read validation are the
ones that run on the device.

| | ROMs | |
|---|---:|---|
| scanned | 2076 | |
| NO_MATCH | 1662 | no match -- costs nothing (80.1%) |
| OK | 413 | recognizer installs a loop (19.9%) |
| status | 1 | status (0.0%) |

Of the 413 installs: 317 LoROM, 96 HiROM. Sites found per cartridge: 1x272, 2x60, 3x23, 4x11, 5x8, 6x3, 7x2, 8x6, 9x3, 10x6, 11x1, 13x1, 14x3, 15x1, 16x3, 17x2, 18x1, 19x1, 20x1, 36x1, 37x1, 44x2, 49x1.

## Hash gate

Both arms of the M7 rig -- the Thumb-2 engine the device runs -- over every
cartridge the recognizer installs into, 300 frames each, comparing the state
and audio hashes with the feature off and on.

| verdict | cartridges |
|---|---:|
| SAME | 413 |

## How often the loop actually runs

`laps` counts replayed iterations in a 300-frame window from cold boot. That
window understates play: A Link to the Past replays 730 laps/frame in a boot
window and 3,976 in its play scene, a factor of 5.4. Read this as *whether* the
loop is live, not as the size of the prize.

- fired at least once: **106** of 413 gated
- silent in the boot window: 307

| cartridge | laps / 300 frames | site |
|---|---:|---|
| 드래곤즈 매직 (Dragon's Magic).smc | 2,197,732 | `00:f392/f394` |
| 파치오군 스페셜 (Pachio-kun Special).smc | 2,119,859 | `00:80a9/80ab` |
| 마루코는 아홉살 - 의기양양 365일의 권 (Chibi Maruko-chan - Harikir | 2,022,717 | `00:81ba/81bc` |
| 드래곤스 레어 (Dragon's Lair).smc | 1,998,368 | `00:f081/f083` |
| 헤베레케 포포이토 (Hebereke's Popoitto).smc | 1,862,057 | `00:8758/875a` |
| 하나후다 왕 (Hanafuda Ou).smc | 1,804,012 | `00:8051/8053` |
| 암흑의 바다 해적 (Pirates of Dark Water, The).smc | 1,784,410 | `00:a97b/a97d` |
| 일발역전 - 경마 경륜 경정 (Ippatsu Gyakuten).smc | 1,758,027 | `00:8034/8036` |
| 커비의 고스트 트랩 (Kirby's Ghost Trap).smc | 1,700,961 | `00:80cd/80cf` |
| 다이내믹 스타디움 (Dynamic Stadium).smc | 1,690,419 | `00:807d/807f` |
| 오셀로 월드 (Othello World).smc | 1,649,323 | `00:852a/852c` |
| 포포잇토 헤베레케 (Popoitto Hebereke).smc | 1,633,100 | `00:8755/8757` |
| 카토 1-2-3 9단 쇼기 클럽 (Katou Hifumi Kudan - Shougi Club) | 1,623,118 | `c0:093a/093c` |
| 데이비드 크레인의 어메이징 테니스 (David Crane's Amazing Tennis).sm | 1,614,515 | `0e:f381/f383` |
| ABC 먼데이 나이트 풋볼 (ABC Monday Night Football).smc | 1,546,633 | `00:8332/8334` |

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
