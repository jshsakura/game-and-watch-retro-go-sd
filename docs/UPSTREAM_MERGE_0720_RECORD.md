# Upstream merge record — 21 commits from sylverb/game-and-watch-retro-go-sd

Merged 2026-07-20. Base `66eaa937`, upstream tip `96473b73`.

Written so the diff is reviewable from *both* sides: what upstream sent, what we
had to adapt, and — separately — what was already broken here and merely became
visible. Upstream commits keep Sylverb's authorship and original messages; every
adaptation is its own commit on top rather than being folded into his.

## Policy applied

Where upstream's design and ours disagreed, **upstream wins and we adapt** (this
fork is downstream, not a competing implementation). The one place we kept our
own value is `MAX_EMULATORS`: upstream uses 21, we use 31 because we ship 31
cores. We adopted upstream's *mechanism* (dynamic `ahb_calloc`) and kept only the
count.

## Design conflicts and how they were resolved

| Conflict | Upstream's design | What we did |
|---|---|---|
| `emulators[]`/`systems[]` fixed array vs dynamic (`8caa3e45`) | `ahb_calloc` at `emulators_init`, `MAX_EMULATORS 21` | Adopted `ahb_calloc`; kept our count 31 (verified: exactly 31 `add_emulator()` calls) |
| Fixed `_Heap_Size` vs heap-fills-DTCM (`f25539a2`) | Heap fills whatever `.data`/`.bss` leave | Adopted. Our hand-tuned `_Heap_Size = 81K` was **already dead** — our own `.heap` linker section had independently implemented the same dynamic-fill scheme, so the constant was unreferenced. Confirmed by grep before deleting. |
| `rg_calloc` (debug allocator) dropped (`8caa3e45`) | `-DDEBUG_RG_ALLOC` removed; `gui.c`'s `tab_t` moved to `ahb_calloc` | Followed: our `rg_favorites.c` was the last caller; switched it to `ahb_calloc`, which matches its actual lifetime (allocated in `emulators_init`, invalidated by the `ahb_init()` before each core launch — same as `emulators[]`) |
| `odroid_system_emu_init` 6→7 args (`8caa3e45`) | added `cheat_update_cb` | Added `NULL` at 12 fork-only call sites. Behaviour-preserving: the parameter did not exist before, so these cores never had a handler — and it matches what upstream passes for its own handler-less cores (tama, nes). |

## Measured effect (docker release build, canonical flag set)

| Budget | Before | After | Δ |
|---|---:|---:|---:|
| DTCM heap | 82,944 | 91,836 | **+8,892** |
| AHB static reserved | 34,976 | 34,976 | 0 |
| AHB dynamic free | 87,904 | 87,904 | 0 |

The DTCM gain is the point of upstream's three memory commits, and it lands: our
`_Heap_Size` was trimmed 85→81 KB precisely *because* Atari Lynx grew
`emulators[]` in DTCM `.bss`. Moving that array to the AHB pool gives the space
back. AHB is unchanged because the array is a *runtime* `ahb_calloc` and
`ahb_init()` wipes the pool before any core loads — so 32X's ~84 KB `Draw2FB`
allocation still sees the same headroom it did before (1,888 bytes spare, a
pre-existing margin this merge neither improved nor consumed).

## Things upstream may want to know (fork-side, not defects in his tree)

- **`retro-go-stm32` submodule diverged both ways.** Upstream added a cheat
  callback + PCE-DUO identification; we had an idle-power-off rule and an
  `strncpy` bounds fix. Reconciled inside the submodule (clean merge, different
  files); all four commits verified as ancestors of the result.
- **`gw_flash_alloc.c`**: upstream's `2116bedc` absorbed an older snapshot of our
  flash-cache work. We kept our newer `find_write_slot()` fix and the
  `relocate_cb` hook (XIP support), both of which postdate what he took. His
  large-erase-size speedup is retained. Worth a look upstream: our
  `find_write_slot` version fixes a case where a cache write could land on a file
  the running core was still reading.

## Pre-existing problems this verification exposed (NOT caused by the merge)

Recorded separately so they are not mistaken for merge fallout:

1. **`external/sm` gitlink `4124a939` is not fetchable from its remote**
   ("upload-pack: not our ref"). A clean clone or CI run cannot check this
   submodule out; it only worked here because the commit exists in a local
   checkout. **This is a release/CI blocker and needs pushing.** Same class as
   the earlier picodrive incident.
2. **`tools/sm_harness/glue.c` lacked an `apu_run` stub.** `device_parity.sh`
   excludes `main_sm.c` (which stubs `apu_run` on device), so the parity link
   reported it as a symbol that would alias another core's overlay. Proven
   pre-existing: the stub was already in `main_sm.c` at base `66eaa937`, the
   `external/sm` gitlink is byte-identical before and after, and the merge's only
   edit to `main_sm.c` is the `emu_init` argument. It stayed hidden because that
   test SKIPs whenever `external/sm` is absent — which is always in CI. Fixed
   here; the firmware itself always linked.

## Verification performed

- Docker release build, canonical flag set (`package.yml`), from a clean `build/`
- Non-HLE build (`SNES_SMW_HLE` omitted) built **clean**, not incrementally, since
  object files depend on the Makefiles rather than on flag *values* — an
  incremental flag flip would have silently linked stale objects
- `tests/run.sh`
- `tools/gnw_hw_harness/run.sh` (updated for the dynamic heap: `_Heap_Size` is
  gone, so the declared-vs-actual cross-check was replaced with
  `_heap_end <= _heap_limit`, keeping the stack-collision property it defended)
- Fork features confirmed in the linked ELF by symbol: clock, alarm, favorites,
  system grid, boot rescue, coverflow, CJK i18n
- All 31 emulator registrations present, list byte-identical to pre-merge,
  favorites still first
