# Upstream merge scoping — 21 commits behind `upstream/main`

**Scoping only — no merge was executed or committed.** Real conflict data
below comes from an actual `git merge --no-commit --no-ff upstream/main` run
in an isolated detached worktree (`git worktree add --detach`), then
`git merge --abort` + `git worktree remove`. The user's working branch
(`perf/32x-histogram`) was never touched — confirmed via `git worktree list`
before and after. This was chosen deliberately over static inference, per
today's rule: don't assert "no conflicts" without testing it.

## ① The 21 commits, categorized

| Commit | Type | Summary | Do we want it? |
|---|---|---|---|
| `4d88c969` | bugfix | PCE/PCECD: CD volume, identify as DUO, gfx-resolution fixes, Toy Shop Boys | Yes — bugs in a core we ship |
| `639c2907` | refactor | Remove unused `extract_system` function | Yes, trivial |
| `8caa3e45` | **memory refactor + feature** | Move non-emulation data from DTCM to AHBM; cheat-update rework (removes need for system struct in-game) | **Yes, high value** — directly relevant to our own memory-budget work |
| `651c2e83` | bugfix | LCD: fix garbage display after restart | Yes, if we have the same bug (not yet checked) |
| `569fd99f` | chore | "missing updates from previous commits" (fixup) | Yes, trivial |
| `4bbdcfd2` | bugfix | LCD: proper deinit | Yes |
| `c1be726a` | **memory refactor** | Only store one non-UK language in DTCRAM | Yes — same spirit as our own i18n footprint work, DTCM not intflash but same category of care |
| `f25539a2` | **memory refactor** | Heap fills all free DTCM instead of fixed `_Heap_Size` | **Yes, high value, but conflicts with our own manual DTCM tuning** — see ② |
| `31a08000` | buildsystem/bugfix | PCE ITCM usage in size script; flash-only cheats compile fix | Yes |
| `1ebcd9b3` | buildsystem | Covers handling gated by `SD_CARD=0` flag instead of folder existence | Yes |
| `2116bedc` | bugfix | Flash cache: **merged fixes from our own fork** (large-erase-size speedup, min-erase-size fix) | Yes — this is upstream absorbing *our* earlier work back; should be near-trivially compatible |
| `d50b8139` | feature/bugfix | PCECD: subfolder support, faster folder parsing | Yes |
| `985454c8` | bugfix | PCECD: savestate path >100 chars | Yes |
| `1ed0094e` | bugfix | Only save current tab if in launcher (invalid in emulators) | Yes |
| `bc10fdf0` | asset fix | Proper Lynx logo | Yes |
| `b29e6e19` | bugfix | Null out `emulators`/`systems` before starting an emulator | Yes — small stability fix |
| `5399bd92` | feature | GB: palette change preview | Yes |
| `d1191352` | bugfix | tgbdual: Daedalian Opus / Altered Space / Street Fighter 2 fixes (submodule bump) | Yes |
| `f098d735` | feature | GB: Super Gameboy mode option | Yes |
| `51ac3a3b` | feature | GB: SGB borders | Yes |
| `96473b73` | buildsystem/DX | Docker build now interruptible with Ctrl-C | Yes, trivial, low-risk |

No clearly-unwanted commits in this batch — this isn't a "cherry-pick the
good ones, skip the rest" situation. The three memory-refactor commits
(`8caa3e45`, `c1be726a`, `f25539a2`) are the highest-value overlap with our
own current work, and also the ones that conflict (see ②).

## ② Real conflict data (from the actual test merge)

**9 files, 16 conflict hunks, 1 submodule conflict.** `Makefile` and
`Makefile.common` — both heavily diverged (492 / 512 lines) — **merged
cleanly, zero conflicts.** `STM32H7B0VBTx_FLASH.ld` also merged cleanly
(only `STM32H7B0VBTx_SDCARD.ld`, the one we've actually built new overlay
blocks into, conflicted).

| File | Conflict hunks | Our divergence | What's actually conflicting |
|---|---:|---:|---|
| `Core/Src/retro-go/rg_emulators.c` | **6** | 733 lines | Upstream converted `emulators[]`/`systems[]` from fixed `MAX_EMULATORS`-sized static arrays to dynamically-allocated pointers (part of the DTCM refactor, `f25539a2`/`8caa3e45`). We've been manually bumping `MAX_EMULATORS` (19→31) for every core we've added (NGP, WonderSwan, Lynx, PCE-CD, Odyssey2, ZX Spectrum, C64, game.com, VB, GBA, SNES, 32X). **This is a design reconciliation, not a text merge** — adopting upstream's dynamic scheme means re-verifying every one of our added cores still registers correctly. |
| `Core/Src/gw_flash_alloc.c` | 2 | — | Not inspected in detail (scoping budget) |
| `Core/Src/retro-go/rg_utils.c` | 2 | 4 lines | Small file, small divergence, likely quick |
| `Core/Src/porting/odroid_overlay.c` | 1 | 172 lines | Not inspected in detail |
| `Core/Src/retro-go/gui.c` | 1 | 550 lines | Not inspected in detail |
| `Core/Src/retro-go/gw_firmware_abi.c` | 1 | small | Not inspected in detail |
| `Core/Src/retro-go/rg_logos.c` | 1 | 1,581 lines | Single hunk despite huge divergence — likely both sides added icons in the same append region |
| `Core/Src/syscalls.c` | 1 | 157 lines | Not inspected in detail |
| **`STM32H7B0VBTx_SDCARD.ld`** | 1 | 700 lines | Upstream's dynamic-heap refactor (`f25539a2`) removes our fixed `_Heap_Size = 81 * 1024` (itself hand-tuned after Lynx's `MAX_EMULATORS` growth) in favor of "heap fills whatever's left after `.data`/`.bss`". **Same root cause as the `rg_emulators.c` conflict** — these two are the same design change, need to be resolved together, not independently. |
| **`retro-go-stm32`** (submodule) | — | gitlink | See below — **this one is easier than it looks** |

### The `retro-go-stm32` submodule conflict, characterized precisely

Not a "parallel invention" nightmare like today's 32X/picodrive reconciliation.
Both sides moved from a common ancestor (`c2531bd`, already-merged):

- **Upstream added 2 commits we don't have**: `9fa8f81` (cheat update
  callback — this is `8caa3e45`'s submodule-side half) and `0fb02a7`
  (identify as PC Engine DUO — `4d88c969`'s submodule-side half).
- **We added 2 commits upstream doesn't have**: `a9c3c0c` (idle power-off
  rule — our own boot-rescue-adjacent work) and `a157052` (bound-check
  `strncpy` in `emulator_build_file_object`/`favorites_save` — our own
  hardening fix).

Different files, different concerns (cheat callbacks/PCE-DUO vs. idle-power
rule/strncpy bounds) — a normal 4-way merge inside the submodule's own repo,
not a redesign. Low-to-medium effort, not high.

## ③ Submodule pointer check (the "picodrive-style" risk category)

Checked every submodule the 21 commits touch:

| Submodule | Status |
|---|---|
| `external/tgbdual-go` | We never moved it — upstream's pointer is a clean fast-forward, zero conflict |
| `external/blueMSX-go` | Same — clean fast-forward |
| `retro-go-stm32` | **Both sides moved independently** — see ② above, characterized as low-medium effort |

Only one submodule needs real reconciliation, and it's the tractable kind.

## ④ Effort/risk estimate

**미검증 (unverified) — this is a size estimate from real conflict-hunk
counts, not a byte/time measurement**, flagged per today's rule after the
−6.02 KB-vs−2.2 KB miss:

- 7 of 9 conflicted files: single-hunk, small-to-medium divergence — likely
  quick (minutes each) once someone reads both sides.
  **미검증**: didn't inspect their actual hunk content beyond `rg_emulators.c`
  and `STM32H7B0VBTx_SDCARD.ld`.
- `rg_emulators.c` (6 hunks) + `STM32H7B0VBTx_SDCARD.ld` (1 hunk, same root
  cause): the real work — deciding whether to adopt upstream's dynamic
  `emulators[]`/heap-sizing scheme, then re-verifying it against every one
  of our 12 added cores. This is the part that needs a human/device-level
  decision, not just a text merge.
- `retro-go-stm32` submodule: low-medium, normal 4-commit reconciliation.
- **Rough shape, not a number**: most of the file list is an afternoon of
  careful reading; the `rg_emulators.c`/linker-script pair is the one item
  that could genuinely take a full day if the dynamic-array redesign is
  adopted rather than kept parallel.

## ⑤ Merge strategy — recommendation

Three options, with tradeoffs:

- **All-at-once (`git merge upstream/main`)**: what was tested above. Pro:
  one conflict-resolution pass, one verification pass. Con: bundles the
  hard conflict (`rg_emulators.c`/heap redesign) with 20 low-risk commits —
  if something's wrong post-merge, has to un-bundle 21 commits' worth of
  change to bisect it.
- **Staged by category**: land the trivial/bugfix batch first (PCECD fixes,
  tgbdual fixes, LCD fixes, GB features, docker Ctrl-C — 17 of 21 commits,
  none of which touch the two conflicted files) as one merge/cherry-pick
  batch; handle the memory-refactor trio (`8caa3e45`, `c1be726a`,
  `f25539a2`) as a separate, deliberate second pass once there's time to
  actually decide on the dynamic-array question. **Recommended** — isolates
  the one real design decision from 17 commits of "obviously want this."
- **Cherry-pick only what's wanted**: given nothing here looks unwanted,
  this doesn't save real effort over staged merging and loses the clean
  history a real merge preserves — not recommended unless something in the
  batch turns out to be actively unwanted on closer look.

**Recommendation: staged, not all-at-once, not cherry-pick.** Land the 17
non-conflicting commits as a batch merge/rebase first (verify, ship), then
treat the 3 memory-refactor commits as their own scoped task with the actual
design question (adopt dynamic `emulators[]`+heap, or keep parallel to it)
decided deliberately — not resolved as a side effect of a bigger merge.

## ⑥ Verification gate — what proves "didn't break anything"

- **Docker build** (Opus's job, per standing rule) — link success, intflash/
  extflash size sanity, no `ASSERT` failures (BSS overflow, DTCM heap, etc.
  — several of the 21 commits directly touch these budgets).
- **`tests/run.sh`** — the host-buildable suite; anything it covers
  (boot-rescue, idle-timeout wiring, etc.) should stay green.
- **Per-core rig hashes where they exist**: SNES (`STATEHASH`/`AUDIOHASH`
  bit-identical gates already used this session), any other core with a
  device-parity harness per `docs/HARNESSES.md`.
- **Device verification, specifically for**: PCE/PCECD (4 of the 21 commits
  touch it — CD volume, DUO identification, savestate paths, subfolder
  support), GB/GBC via tgbdual (4 commits — palette preview, SGB mode/
  borders, several core fixes), Lynx (logo asset), LCD restart/deinit
  behavior, and — if the dynamic-array/heap redesign is adopted — **every
  core we've added since the fork point**, since `MAX_EMULATORS` and the
  DTCM heap are exactly the kind of budget where a silent overflow only
  shows up at runtime on hardware (see `CLAUDE.md`'s own warning about this
  class of bug).

## ⑦ Cost of not merging

Divergence compounds: 21 commits now, arrived over some period of upstream
development we're not tracking day-to-day. The longer this waits, the
larger the eventual batch (this doc's own effort estimate assumed 21 — a
future "50 behind" scoping pass would have proportionally more conflict
surface, not linearly more but likely worse, since `rg_emulators.c`/the
linker script are the files most likely to keep accumulating both-sides
changes given they're where both forks are most actively adding cores/
features). We are also 1,074 commits *ahead* — functionally a soft fork
already — so "never merge again" is a real available choice, but it means
permanently forgoing upstream's bug fixes for cores we still ship (PCE, GB/
GBC, Lynx) and re-solving problems upstream has already solved (dynamic
heap sizing is a real answer to the exact "hand-tune `MAX_EMULATORS` every
time we add a core" pattern our own comment in `rg_emulators.c` documents).
