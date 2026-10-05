# Upstream contribution tracker — historical 2026-08-17 matrix

Preserved from [issue #11](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/11) during the 2026-10-02 reconciliation.
These upstream adoption checks and fork-state classifications belong to that earlier
review. Use [ISSUE_STATUS.md](../ISSUE_STATUS.md) and live #11 for the current queue.

This issue records which parts of the `testbed` fork are in a state where
[`sylverb/game-and-watch-retro-go-sd`](https://github.com/sylverb/game-and-watch-retro-go-sd)
could take them if the maintainer ever wanted to, and what each one would still need
first. It is a record for our side, not a queue of things being pushed at anyone — the
judgement of whether any of it belongs upstream is the maintainer's, and nothing here is
offered as finished work.

The fork is AI-assisted and tested by one person on one device. That is the standing
caveat on every row below.

*Last reconciled against the tree on 2026-08-17. Where a row and the fork's own
`docs/` disagree, `docs/` is right and this issue is stale.*

---

## Already upstream — the cycle works, and the bottleneck was our structure

Five pieces have made it across, four of them credited to the fork by name. Worth
recording because it reframes everything below: adoption has not been limited by the
maintainer's willingness.

| upstream commit | what |
|---|---|
| `2116bedc` | Flash cache — large erase size, and caching for chips whose minimum erase exceeds 16 KB. Credited "merged fixed from jshsakura's fork" |
| `61daaedc` | Atari Lynx (handy-go). Upstream's `.gitmodules` points at `jshsakura/handy-go` at the same commit hash |
| `45b9196f` | PC Engine CD, "from jshsakura's fork" |
| `cb488250` | Favorites management, "from jshsakura + some improvements" |
| `2ad10327` | Experimental GBA via gpSP (SD-card only) |

## One open loop

**Music player (`sylverb#84`)** — closed **by us** on 2026-06-25, not by the maintainer.
The maintainer's last word was positive and he asked for time. It was withdrawn because
the branch had picked up things that only made sense for personal use, with the stated
intention of tidying it and re-opening. That has not happened. If the app is still wanted,
the work is a lean re-cut, not new development.

---

## Systems in the fork

Grouped by what an upstream reader would need to know, not by how interesting they were
to build. `docs/` and the per-core `CLAUDE.md` files carry the real detail.

### Playing on device, self-contained

| System | Core | Note relevant upstream |
|---|---|---|
| WonderSwan / Color | oswan | 75 fps; the lever was cycle-accurate idle skip. 27 rendering glitches closed |
| Neo Geo Pocket / Color | RACE | Savestate sound-resume needed `ngpRunning` in the snapshot |
| Atari Lynx | handy-go | *Already upstream* — 512 KB carts run XIP from QSPI |
| PC Engine / CD | pce | *Already upstream* — BRAM, CD-DA and ADPCM complete on device since |
| Virtual Boy | mednafen-derived | ~65–70% speed; the bottleneck differs per game, so there is no single lever |
| Pokémon Mini, Supervision, Tamagotchi, Videopac, Amstrad CPC, game.com | various | Smaller cores; see `docs/` |
| GBA | gpSP | *Already upstream (experimental)*. Ruby/Emerald full speed here; the lever was an HLE of the M4A mixer, which is 27–60% of guest time |

### Deep work, large diff, fork-shaped

- **SNES** — the most heavily modified core in the tree, and the least separable. Wait-loop
  baking, a fetch-page cache rewritten around per-bank bases (a 3 MB cartridge previously
  had no cache at all), a Thumb-2 65816 and SPC700, and now two coprocessor HLEs. Any of
  it upstream would be a conversation about *one* piece, not a merge.
  - **DSP-1 HLE** — Mario Kart, Pilotwings, Suzuka 8 Hours, Battle Racers.
  - **Cx4 HLE** — clean-room, docs-based; *Mega Man X2* and *X3* boot and play. See #44,
    including why the first implementation had to be discarded over its licence, and the
    six cartridge-controlled writes that were leaving the chip's 8 KB RAM.
- **Native SNES ports** (Super Metroid, SMW, Zelda 3) — these are overlays sharing one RAM
  address with everything else, which is where the fork's cross-overlay alias guard came
  from.

### Experimental / not settled

- **Sega 32X** — D32XR runs on hardware. The numbers are in `docs/32X_NEXT_SESSION.md` and
  `docs/32X_CLOSED.md`, with one warning that matters more than any of them: **every 32X
  fps figure published anywhere, by us, is the attract demo rather than gameplay** unless
  it says otherwise. Reusing an attract verdict for gameplay cost a closed optimisation
  axis its closure — the clock floor measured +0.6% on attract and +12.0% on the gameplay
  anchor.
- **Homebrew apps** — video player (MJPEG/AVI, #8), media browser, clock with alarms,
  system-grid home. Fork features; none of them shaped for upstream.

### Closed, with the reason recorded

| | why |
|---|---|
| Sega CD | An accurate implementation needs 768 KB where `RAM_EMU` is 724. Physical, not an optimisation problem |
| CPS-1 | Render parity proven on host; the device never got past the title. Abandoned by choice |
| SuperFX | Fits in RAM (~114 KB). Closed on CPU — a RISC core interpreted per frame, on top of a 65816 interpreter that already owns most of the budget |
| SA-1 | 256 KB of BW-RAM *and* a second 65816 |
| DOOM / Duke / Wolfenstein 3D | XIP-veneer and OSPI 3-region execution corruption, not reproducible on host |

---

## Core-agnostic things, offered as information only

These are not features and they are not PRs. Each is a small piece of shared machinery
where the fork found the general case, and each is written up so it can be read rather
than taken.

- **A shared default carries one core's circumstances.** `common_emu_frame_loop()`'s
  overload guard forced one drawn frame in four — a floor chosen when the core in front of
  someone had a 17.65 ms draw. Every core inherited it. On 32X the draw is 1.9% of the
  frame, so the guard was discarding three quarters of the frames for nothing. It is a
  per-core value in the fork now, default unchanged. #43.
- **Overlays alias silently.** A core referencing a global only another core defines gets
  bound to that core's address, quietly, and then reads its own unrelated data. This cost
  three releases. The fork namespaces each core's globals and fails the link if any core
  reaches into another's overlay, confirmed by disassembly so dead references do not trip
  it.
- **Adding an APPID resets every user's settings**, because `/CONFIG` is a raw dump of a
  struct containing `app[APPID_COUNT]`.
- **A hung boot used to need a flat battery.** The fork counts consecutive failed boots in
  an RTC backup register and stops the third at a rescue screen before SD, config and
  auto-resume.
- **`lang_t` is indexed by position**, so a string may only be appended; a retired one has
  to keep its slot.

## Keeping the diff takeable

The standing rule in this fork is to never make upstream work around us: we route around
upstream instead, annotate anywhere the fork deliberately differs, and append our
additions after upstream's in any position-indexed table so the next merge stays clean.
`tests/run.sh` has a merge-hygiene guard for the class of break that already happened once
(an auto-merge duplicating a position-indexed logo enum).

---

Related: #8 (video player), #9 (debugging post-mortem), #10 (WonderSwan accuracy),
#12 (harness engineering notes), #19 (small upstream-ready fixes), #41, #42, #43 (SNES and
shared-machinery findings), #44 (Cx4).
