# N-SPC HLE proof-of-concept

Answers the question that gates the whole "generalize spc_player" plan: **can
Super Metroid's native N-SPC engine (`external/sm/src/spc_player.c`) render a
DIFFERENT N-SPC game's music**, given only that game's uploaded APU RAM and the
three addresses our survey recovers?

**Yes — for standard-variant N-SPC games.** Proven end-to-end on real ROMs:

| ROM | variant | result |
|---|---|---|
| Zelda: A Link to the Past | std | song 2 **plays** (peak 12006, RMS 2162, 96% non-zero) |
| Super Mario Kart | std | song 2 **plays** (peak 11732) |
| Gradius III, TMNT IV | GD3 (Konami fork) | silent — fork needs its own vcmd table |
| DKC2 | SMW | silent — 0xDA-base vcmd set |

SM's engine *is* the standard N-SPC engine, so same-variant games render with
**zero engine changes** — only the recovered song-list / instrument-table / DIR
addresses. Variant forks (GD3, SMW-era, Tose, Intelli) stay silent until their
per-variant vcmd handling is added — the bounded follow-on work in the plan.

## How it works, and the one rule it respects

`spc_player.c` hardcodes four ARAM addresses (song list `0x5820`, current-song
`0x581e`, instrument table `0x6c00`, DSP DIR page `0x6d`). We must NOT edit the
submodule, so `build.sh` **sed-rewrites those four literals** into runtime-config
macros (`nspc_config.h`) in a *generated copy* — the submodule file is untouched.

`nspc_poc.c`:
1. boots the ROM in the emulated core (survey boot loop) so it uploads its
   driver+data to APU RAM,
2. recovers song/instr/dir via `nspc_extract` (same code as the survey),
3. hands a fresh `SpcPlayer` that ARAM + those addresses (`g_nspc_cfg`),
4. kicks a song through the standard port protocol and runs the native engine,
5. dumps a 16 kHz WAV and reports peak amplitude.

```bash
bash tools/nspc_hle/build.sh
NSPC_WAV=/tmp/x.wav /tmp/nspc_hle_build/nspc_poc <rom> [songid=1] [frames=600]
```

## Build gotcha worth remembering

`external/sm/src/features.h` shadows glibc's `<features.h>` if `src` is on the
`-I` path — poisoning the entire libc header chain (size_t/uint8_t "undefined").
Use `-iquote external/sm/src` (affects only quoted includes) for the SM sources.

## Not yet

Music-only (SFX engine is SM-specific, left as-is). Variant vcmd tables (GD3/SMW/
Tose) not implemented. Not wired into the device — this is a host feasibility rig.

## Variant support (2026-07-15 night, second pass)

The engine now speaks three dialects, adapted at stream-read time (generated
copy only; `gen_variant.py` applies 16 exact-anchor rewrites on top of the
address sed — a miss aborts the build):

| dialect | games proven (peak) | how |
|---|---|---|
| std / YI 0xE0 | Zelda 12006 · Mario Kart 11732 · Gundam Wing 29096 · Gadurin 16021 · Darius Twin 3742 | engine as-is |
| SMW-era 0xDA | Super Mario World 8803 (songs 2–8) · Live A Live 10557 | REORDERED vcmd remap (not a base shift: pitch-slide sits at 0xDD), 5-byte instruments, tie/rest 0xC6/0xC7, percussion 0xD0–0xD9, SMW note tables |
| GD3 / Konami | Gradius III 13477 · TMNT IV 5206 · Castlevania IV 8732 | std base + overrides (loop start/end + ADSR/GAIN handled natively, unknowns skipped by length), KonamiBase pointers, per-SRCN tuning tables |

Not implemented: **Tose** (~25 ROMs — own YSFR dialect, extraction fails
cleanly: chOK=0) and **Intelli** (4 ROMs). They stay silent-with-log, which is
the LLE-fallback boundary.

Hard-won specifics:

- **The InitSectionPtr patterns must be ZP-constrained.** VGMTrans *patches*
  the section-pointer zero-page byte into the pattern before matching; our
  extractor wildcards it, and unconstrained it hits the game's SFX pointer
  tables first (SMW: 3 matches, first one wrong -> silence). `sig_pos_zp()`
  requires the match to store to the ZP that `ptnIncSectionPtr` walks.
- **`SpcPlayer_Initialize` reads SM's zero-page layout out of the foreign
  ARAM** (`CopyVariablesFromRam`) — garbage that may or may not gate all DSP
  writes depending on *boot frame count* (SMW played at boot=60, silent at
  boot=300 via `is_chan_on`). The PoC clears engine state after Initialize;
  a shipped integration must do the same.
- **Guards, not hangs**: a stream read with the wrong dialect is garbage; the
  engine's `WantWriteKof` readahead, `next_phrase` playlist walk and
  fast-forward loop are now iteration-bounded in the generated copy.
- Song id conventions vary: table entry 0 may be a dummy (SMW: songs are 2–8)
  and banks load lazily (TMNT: only songs 1–2 exist, and only after ~600 boot
  frames). A shipped player should scan/validate, not assume id 1.
