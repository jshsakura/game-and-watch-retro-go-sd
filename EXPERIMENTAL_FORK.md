# Experimental Lab Fork

> **This is a personal, experimental fork** of
> [sylverb/game-and-watch-retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd).
> It tracks upstream but carries extra, in-progress work that is **not** in
> the official build. Use at your own risk.

## What differs from upstream

The single maintained list is in the
**[README — How this fork differs from upstream](README.md#how-this-fork-differs-from-upstream)**.
Release notes list only what changed in each build. (A feature table used to live here
too; two lists drifted apart, so now there is one.)

The debugging history behind the features — what failed, what the actual root causes
were, and how each finding was verified (host harness vs. on-device SD log) — is in
[docs/UPSTREAM_ENGINEERING_NOTES.md](docs/UPSTREAM_ENGINEERING_NOTES.md).

## Performance notes (WonderSwan)

WonderSwan runs the V30MZ CPU, which is heavier than the Neo Geo Pocket's
TLCS-900. Light/medium games (RPG, puzzle, board) run near full speed; the
heaviest sprite-driven action games (e.g. Rockman EXE) run at roughly 40% speed.
This is the hardware ceiling for an interpreter on this MCU — no safe
optimization closes it without a dynamic recompiler.

Applied CPU optimizations (all minimal-diff):
- Inlined the hot memory-read path into the CPU core
- Direct-branch for internal-RAM (VRAM) writes
- Cached the code-segment base per instruction in the fetch path
- `-O3` on the core, render-skip on dropped frames

Deliberately **not** done (negligible gain and/or save-integrity risk):
- External-flash (XIP) caching — no measurable gain, cache-coherency risk
- SD overclock — instability, doesn't help runtime (ROMs run from flash, not SD)

## Build / install

CI builds `retro-go_update.bin` on push; tagged builds (`testbed-full-*`) are attached
to Releases. Flash `retro-go_update.bin` with the normal G&W update procedure — the
flashing procedure also pushes the matching SD support files (`/cores/*`,
`/bios/logo.bin`, homebrew overlays) automatically.

## Relationship to upstream

**`testbed`** is the integration branch (this repo's default): all features combined,
what the releases are built from. `main` mirrors `sylverb/main` and is synced
periodically; feature work happens on topic branches and lands in `testbed`.

The fork is introduced to upstream — with the hope that a small part of it can
contribute back someday — in
[sylverb's discussions #94](https://github.com/sylverb/game-and-watch-retro-go-sd/discussions/94).
