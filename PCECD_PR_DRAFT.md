# PC Engine CD (Super CD-ROM2) support — draft

This is a draft for adding PC Engine CD-ROM² / Super CD-ROM² support to the
SD-card build, on top of the existing HuCard `pce-go` core. It is opened as a
draft because the submodule side still needs a push (see below) and because a
couple of design points are better decided by you than guessed by me.

The disc is streamed from the SD card, so this is an **SD_CARD=1 only** feature.
Flash-only (non-SD) builds compile the CD layer out and link tiny no-op stubs
instead, so nothing about the non-SD build changes.

## What it does

- Adds a "PC Engine CD" system that lists `*.cue` discs from `/roms/pcecd`.
- Reuses the existing HuC6280/VDC `pce-go` overlay — the CD titles run on the
  same core as HuCard PCE, so there is no second overlay to fit.
- Implements the CD-ROM² hardware on the host side: a SCSI target, the CUE/BIN
  disc layer, ADPCM playback, and CD-DA (redbook audio) mixing.
- Backup RAM (BRAM) is stored at `/data/pcecd.bram`.

## Files changed and why

Main repo:

- `Core/Src/porting/pce/pce_cd.c` / `.h` — CUE/BIN parsing + disc/TOC model,
  data reads, and the CD-DA track playback/seek that feeds the mixer.
- `Core/Src/porting/pce/pce_scsi.c` / `.h` — the `$1800–$18FF` CD-ROM² register
  file as a SCSI target (command phase, data-in, status/message, IRQs).
- `Core/Src/porting/pce/pce_adpcm.c` / `.h` — the `$1808–$180E` ADPCM voice.
- `Core/Src/porting/pce/pce_cd_stubs.c` — no-op replacements for the four
  symbols the core references from its IO decode (`pce_scsi_read/write`,
  `pce_scsi_pc_tick`, `g_pcecd_trace`), linked only when `SD_CARD=0` so the
  flash build still resolves without pulling in the CD stack.
- `Core/Src/porting/pce/main_pce.c` — detects a `.cue` on launch and brings up
  the CD stack, mixes ADPCM + CD-DA into the PCE audio, keeps BRAM, and heals
  the CD file handle across sleep/wake. All of it is under `#if SD_CARD`.
- `Core/Inc/porting/pce/sound_pce.h` — raises `PCE_SAMPLE_RATE` to 44100 so
  CD-DA (44.1kHz on disc) passes through without a decimation stage.
- `Core/Src/retro-go/rg_emulators.c` — registers the "PC Engine CD" emulator
  (SD builds only, reusing the plain PCE logos), dispatches it to the PCE
  overlay, and lists `/roms/pcecd` recursively so per-game folders show their
  `.cue` flat instead of as folders to open. `MAX_EMULATORS` is bumped by one
  only in the `SD_CARD=1` build (DTCM is tight, so the non-SD build stays at 20).
- `Makefile` — `PCE_C_SOURCES` gets the CD sources for `SD_CARD=1` and the stub
  file otherwise.

Submodule (`retro-go-stm32`, the `pce-go` core): 5 commits on top of the pin
currently referenced by `main`, touching only `pce.c` / `h6280.c` / `pce.h`:

- route `$1800` CD-ROM² IO to the host SCSI target,
- a per-instruction PC hook used by the CD timing,
- report the Super System Card signature at `$18C0–$18C7`,
- CD backup RAM in the core,
- hold an IRQ whose vector points at a `BRK` byte (this last one fixed a real
  boot trap on some discs).

## Requirement: system card

CD titles need a Super CD-ROM² system card ROM (the BIOS) present, same as on
real hardware. Without it a `.cue` will not boot.

## Tested titles (on device)

Castlevania: Rondo of Blood (Dracula X), Cotton, Dynastic Hero, and Gate of
Thunder all boot and play with in-game music (CD-DA + ADPCM) and saves. Dynastic
Hero in particular was the one that surfaced the ADPCM and BRK-vector fixes.

## Why SD-only

The disc is far too large to fit in flash, and it is read on demand while the
game runs — that only works with a card to stream from. A flash-only unit has no
disc to point at, so building the CD layer there would only add size for nothing;
hence the stubs. This matches how the non-SD build already drops other
storage-hungry features.

## Submodule handling (needs your hand)

The main-repo PR can only pin a submodule SHA. The 5 core commits above live on a
branch (`pcecd`) in my fork of `retro-go-stm32`; the pin in this branch points at
that tip. For you to build/review the PR, that commit needs to be reachable — I
can push the branch to my fork, or you may prefer I open a small separate PR
against `retro-go-stm32` first so the core diff is reviewed on its own. The 5
commits are linear on top of the exact commit `main` pins today, so it is a clean
fast-forward of core-only changes.

## Open questions for you

1. **Overclock.** On device the CD titles want the CPU boosted (~350MHz) to keep
   SCSI + CD-DA streaming + ADPCM fed at full speed; HuCard runs fine at stock.
   My fork does this with a small per-system boost helper that I left out of this
   changeset to keep it focused (see the note in `main_pce.c`). How would you
   prefer per-system overclock to be expressed here, if at all?
2. **Sample rate.** I raised the shared PCE rate to 44100 for bit-perfect CD-DA;
   it also affects HuCard (a little more PSG/mix work per frame). Happy to gate
   it to CD-only if you would rather HuCard stay at 22050.
3. **Diagnostics.** `main_pce.c` still writes a small `/pcecd_diag.txt` trace on
   disc mount. Useful while bringing this up on more discs; say the word and I
   will strip it.
4. Naming: I used "PC Engine CD" to match the existing "PC Engine" entry — let me
   know if you would rather it read "PC Engine CD-ROM²" / "TurboGrafx-CD".
