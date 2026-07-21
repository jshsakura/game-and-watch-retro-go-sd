# SNES sound-driver survey

Answers one question: **how few sound-driver families cover most of a SNES ROM
library?** — because the plan for a general SNES core is to HLE the heaviest
drivers (the SPC700 sits at the base of the emulator's per-frame cost), and that
only pays off if a handful of drivers cover the library. See
`snes-core-feasibility` memory.

## How it works

A game uploads its sound driver into the APU's 64 KB RAM (ARAM) during boot. We
boot each ROM in the generic core (`external/sm/src/snes`, device settings),
snapshot ARAM, and scan it for the engine-identifying byte patterns that
[VGMTrans](https://github.com/vgmtrans/vgmtrans) (zlib) already carries for ~19
SNES driver families.

Three pieces:

| file | what |
|---|---|
| `extract_sigs.py` | Parses the vendored VGMTrans `*Scanner.cpp` (`vgm_src/`) into `snes_driver_sigs.h` — 138 ARAM patterns. Regenerable; never hand-edit the header. |
| `snes_survey.c` | Boots one ROM (parity-checked event loop copied from `tools/snes_harness/snes_main.c`), scans ARAM, prints every matched `family:signature`. For N-SPC hits it also **recovers the per-ROM engine parameters** (song-list / instrument-table / sample-DIR addresses) via VGMTrans's offset recipe and self-validates them by dereferencing the song list (`chOK` 0–8). These three addresses are exactly what Super Metroid's native player (`spc_player.c`) hardcodes — recovering them per ROM is the bridge to a generalized N-SPC HLE. |
| `classify.py` | Turns a run's TSV into a driver histogram. Splits engine-core signatures from shared idioms (`LoadDIR`/`SetDIR`/instrument-table reads), rolls all N-SPC forks into one HLE bucket, and lists the unmatched/crashed tail. |

## Run

```bash
bash tools/snes_survey/run_survey.sh <rom-dir> [frames=600] [out.tsv]
python3 tools/snes_survey/classify.py <out.tsv>
```

`run_survey.sh` rebuilds the sig header and harness, then surveys every
`.smc/.sfc/.swc/.fig` in the directory. Debug: `SNES_ARAMDUMP=/tmp/x.aram
<survey> <rom> <frames>` writes the raw ARAM snapshot.

## Result on the local 150-ROM set (2026-07-15)

- **N-SPC = 71% of bootable+classified ROMs** (Nintendo's Kankichi engine, all
  forks). Sub-variants: YoshisIsland 27, Tose 24, SMW-era 24, standard 10,
  Konami-GD3 5, Intelli 4. → **HLE N-SPC first.**
- Custom engines: Capcom 10, Neverland 3, Falcom 2, Rare 1, Konami-native 1,
  Prism 1.
- ~12% crash on boot — enhancement chips the core rejects (SA-1/SuperFX/Cx4/
  S-DD1: Kirby, SMRPG, Star Fox, Mega Man X2/3, Star Ocean). Out of scope.
- ~13% render fine but match no signature (even at 3000 frames, ARAM 63–96%
  full) — drivers VGMTrans does not catalog: Chunsoft (Shiren), NCS (Langrisser),
  Irem (R-Type), Jaleco (Brawl Brothers). The long tail.

## Notes / limits

- Validated positive: Gradius/TMNT → N-SPC Konami-GD3 fork; DKC → Rare engine
  core. When a signature exists, the scan finds it.
- The unmatched tail is a **coverage** limit of the signature DB, not a scan bug
  (proven: full ARAM, zero of 138 sigs match).
- Boot-crash ROMs cannot be classified here — their drivers would need a core
  that emulates the enhancement chip, which is separately out of scope.
