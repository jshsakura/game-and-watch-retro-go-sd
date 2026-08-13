# Measuring a core change against a cartridge library

How `SNES_SPIN_BAKE` was verified: **2,075 cartridges scanned, 413 gated
hash-identical, 7 measured on hardware.** The feature itself is described in
[SNES_WAIT_LOOP_BAKE.md](SNES_WAIT_LOOP_BAKE.md); this page is about the
measurement, because the method outlives the feature.

Anything that recognises bytes in a ROM — an idle-loop skip, a sound-engine
HLE, a mapper heuristic — is a claim about a library, not about the two
cartridges its author owns. This is what checking that claim looks like, what
the raw data is, and how to re-run it.

## The three layers, and what each one can prove

| layer | question | scale | what it cannot answer |
|---|---|---|---|
| **survey** | what would the recognizer install, across the library? | 2,075 ROMs, 24 s | whether the install is correct |
| **hash gate** | does the emulated machine change on any of them? | 413 ROMs × 300 frames, M7 QEMU rig | whether it is faster |
| **hardware** | what does the player get? | 7 cartridges, savestate scenes | coverage |

No single layer is a verdict. The survey says a fifth of the library is
touched; only the gate says none of that fifth diverges; only the device says
the drawn frames moved.

## The rule that makes the survey worth reading

**The survey links the firmware's own code.** `tools/snes_bake_survey/survey.c`
calls the real `snes_loadRom()` and the real `spin_bake_scan()` — it is 86 lines
of shims and a `printf`.

That is not fussiness. The mapper decision is not a formula, it is a score
across four candidate header positions (`snes_other.c`), and the install then
validates the pc it computed by reading it back through `cart_read()`. A Python
script that reimplemented either would survey **a different program**, and this
tree has paid for that mistake more than once — see the harness lesson in
[CLAUDE.md](../CLAUDE.md) ("a test must compile the file it claims to test").

Same rule in the gate: it runs both arms of the **M7 QEMU rig**, the Thumb-2
engine the device actually executes, not the C interpreter. The charges being
replayed belong to that engine.

## Results

### Survey — 2,075 cartridges

| | ROMs | |
|---|---:|---|
| scanned | 2,075 | |
| NO_MATCH | 1,662 | no match — costs nothing (80.1%) |
| OK | 413 | recognizer installs a loop (19.9%) |

Of the 413 installs: 317 LoROM, 96 HiROM. Sites found per cartridge:
1×272, 2×60, 3×23, 4×11, 5×8, 6×3, 7×2, 8×6, 9×3, 10×6, 11×1, 13×1, 14×3,
15×1, 16×3, 17×2, 18×1, 19×1, 20×1, 36×1, 37×1, 44×2, 49×1.

The 1,662 that match nothing are the important column: a recognizer that
installs nothing must also **cost** nothing, which is why the test sits once per
span rather than once per opcode (measured: a per-opcode test cost 6.6–10.5% of
drawn frames on cartridges where it never fired).

### Hash gate — 413 of 413 identical

| verdict | cartridges |
|---|---:|
| SAME | 413 |
| DIVERGED | 0 |

300 frames per arm, comparing machine-state and audio hashes with the feature
off and on. Zero divergences.

### How often the loop actually runs

`laps` counts replayed iterations in a 300-frame window **from cold boot**.

- fired at least once: **106** of 413
- silent in the boot window: 307

**Read this as _whether_ the loop is live, not as the size of the prize.** A
cold-boot window sits on logos and title screens. A Link to the Past replays
730 laps/frame there and **3,976** in its play scene — a factor of 5.4. The 307
silent cartridges are mostly cartridges whose boot window has not reached the
game yet, not cartridges with nothing to win.

Busiest ten in that window:

| cartridge | laps / 300 frames | site |
|---|---:|---|
| Dragon's Magic | 2,197,732 | `00:f392/f394` |
| Pachio-kun Special | 2,119,859 | `00:80a9/80ab` |
| Chibi Maruko-chan — Harikiri 365-nichi | 2,022,717 | `00:81ba/81bc` |
| Dragon's Lair | 1,998,368 | `00:f081/f083` |
| Hebereke's Popoitto | 1,862,057 | `00:8758/875a` |
| Hanafuda Ou | 1,804,012 | `00:8051/8053` |
| Pirates of Dark Water, The | 1,784,410 | `00:a97b/a97d` |
| Ippatsu Gyakuten | 1,758,027 | `00:8034/8036` |
| Kirby's Ghost Trap | 1,700,961 | `00:80cd/80cf` |
| Dynamic Stadium | 1,690,419 | `00:807d/807f` |

### Hardware — 7 cartridges

Drawn frames per second on savestate scenes the console wrote for itself, both
arms on the same scene. Drawn frames and not fps: most of these sit on the
60.15 fps audio-DMA cap, where the overload guard turns spare time into drawn
frames and the fps counter cannot move.

| cartridge | off | on | | emulated |
|---|---:|---:|---|---:|
| A Link to the Past | 25.90 | **51.03** | **+97.0%** | 60.9 → 60.9 |
| David Crane's Amazing Tennis | 35.10 | **60.68** | **+72.9%** | 61.0 → 60.9 |
| Super Mario World | 23.00 | **33.27** | **+44.7%** | 60.8 → 60.9 |
| Dragon's Magic | 25.30 | **34.17** | **+35.1%** | 53.6 → 54.2 |
| Super Mario Kart | 17.51 | **19.16** | **+9.4%** | 60.0 → 59.9 |
| SD Gundam Generation A | 61.01 | 61.10 | +0.1% | already full rate |
| Super Metroid | 11.71 | 11.70 | −0.1% | no match in ROM |

The last two rows are the ones that make the table evidence rather than
advertising: a cartridge with nothing to win loses nothing, and a cartridge the
recognizer does not match does not move.

## The raw data

Both files are committed, and GitHub renders them as sortable tables.

**[`tools/snes_bake_survey/survey_2k.tsv`](../tools/snes_bake_survey/survey_2k.tsv)** — one row per cartridge scanned.

| column | meaning |
|---|---|
| `name` | file name as scanned |
| `status` | `OK` (loop installed) / `NO_MATCH` / `LOAD_FAIL` |
| `type` | mapper the core chose: 1 = LoROM, 2 = HiROM |
| `sites` | how many wait loops the scan found |
| `site` | the one installed, `bank:pc_load/pc_branch` |
| `dp` | direct-page byte the loop polls |
| `romsize` | bytes |

**[`tools/snes_bake_survey/gate_413.csv`](../tools/snes_bake_survey/gate_413.csv)** — one row per gated cartridge.

| column | meaning |
|---|---|
| `verdict` | `SAME` / `DIVERGED` / `ERROR` |
| `laps` | replayed iterations in the 300-frame window |
| `state_off` / `state_on` | machine-state hash, both arms |
| `audio_off` / `audio_on` | audio hash, both arms |

## Reproducing

```sh
# 1. what the recognizer would install, across a library,
#    using the firmware's own loader and scan
bash tools/snes_bake_survey/run.sh /path/to/roms survey.tsv

# 2. both arms of the M7 rig over every cartridge it installs into,
#    comparing state and audio hashes (resumable; appends to the CSV)
python3 tools/snes_bake_survey/hash_gate.py /path/to/roms \
        --only-installs survey.tsv --out gate.csv

# 3. one markdown report out of both
python3 tools/snes_bake_survey/report.py survey.tsv gate.csv

# 4. on hardware: one line per cartridge, three flashes total
bash tools/gnw_probe/bake_sweep.sh indices.txt
```

Step 4 needs a scene it can resume, and nobody can play a console with a debug
probe soldered to it. `GNW_AUTOSAVE_FRAME=n` has the console boot a ROM, run
*n* deterministic frames and write savestate slot 0 once — **the console makes
its own benchmark scene**. The ROM under test comes from
`/snes_bench_index.txt` on the card, so a sweep is two builds and a reset per
cartridge instead of two container builds per cartridge.

## Two traps this survey walked into

- **A concatenated data file keeps its header.** `survey_2k.tsv` was written in
  two batches, and the second batch's header line landed in the middle of the
  data. It parsed as a cartridge named `name` with status `status`, and the
  published count was 2,076 with a "status: 1" row in the table. Row counts are
  a claim; check that they sum.
- **A wall-clock measurement window is not the same window twice.** One
  unchanged build read 20.73, 25.27, 21.70, 28.04, 24.40, 26.22 and 25.12 drawn
  fps across seven wall-clock samples. `tools/gnw_probe/drawn_ab.sh` takes the
  delta across a fixed number of *emulated* frames instead; every number on this
  page comes from it.

## Where else this applies

The pattern — link the firmware's own recognizer, run it over a library, gate
every hit for equivalence, then measure a handful on hardware — is the standard
for any ROM-pattern feature in this tree. It is what the GBA M4A mixer HLE did
(6 variants over 347 cartridges, bit-exact) and what the SNES audio-HLE gate
failed to do before it shipped a compare that never matched anything, silently.

Related: [HARNESSES.md](HARNESSES.md) (every rig, and which question it
answers) · [OPTIMIZATION_LEDGER.md](OPTIMIZATION_LEDGER.md) (roads already
driven to the end).
