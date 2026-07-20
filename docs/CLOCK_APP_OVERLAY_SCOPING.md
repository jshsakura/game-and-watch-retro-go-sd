# Clock app → `.overlay_clock` — scoping and risk (design only, not implemented)

Goal: recover the ~20 KB the clock app (`rg_clock.c` + `rg_clock_gif.c` +
`rg_clock_album.c` + `rg_clock_alarm_mp3.c` = 19,504 B `.text` + 677 B `.rodata`,
7.7% of the intflash budget) by loading it on demand into `RAM_EMU` the same way
every emulator core already is, instead of keeping it permanently resident.
See `docs/INTFLASH_BUDGET_ANALYSIS.md` for how that number was measured.

This is a design/risk writeup only — **nothing below has been implemented**.

## It is not a drop-in of the emulator-core pattern

Every existing overlay core has exactly one entry point (`app_main_<core>`),
called from exactly one place (`rg_emulators.c`'s `emu_dispatch_t` table), and
nothing the launcher does while the core is unloaded ever calls into it. The
clock app doesn't have that property — three things reach into it from resident
code paths that run whether or not the clock app is currently shown:

| Call site | File:line | Runs when | Needs |
|---|---|---|---|
| `clock_ensure_dirs()` | `rg_main.c:1242` | once, at boot, right after SD mount, before `odroid_system_init`/`emulators_init` | must be resident — overlay isn't stageable yet at this point |
| `clock_gif_reserve()` | `rg_main.c:1243` | once, at boot, same call site | must be resident *if it does anything* — see below |
| `rg_alarm_cache_due()` | `rg_main.c:760` | every second, from the main menu's idle loop, while nothing is loaded in RAM_EMU | must be resident and cheap — this is what decides whether to even trigger a clock load |

Good news on inspection:

- **`rg_alarm_cache_due()` isn't in the clock cluster at all** — it lives in its
  own file, `Core/Src/retro-go/rg_alarm.c`, already separate from
  `rg_clock*.c`. No change needed here; it stays resident exactly as-is, and
  it's already the lightweight "is anything due" check that the menu loop can
  afford to poll every second without touching the heavy clock code.
- **`clock_gif_reserve()` is already a no-op.** Current body (`rg_clock_gif.c:99`):
  ```c
  void clock_gif_reserve(void)
  {
      /* No longer reserves from the emu-RAM pool (that permanently stole ~270KB
       * from the emulators). The decode arena is borrowed from shared_files at
       * load time — see clock_gif_load(). Kept as a no-op so the boot call site
       * is undisturbed. */
  }
  ```
  It does nothing today; the real reservation happens lazily inside
  `clock_gif_load()` (which only runs once the clock app is actually shown).
  This can be deleted outright (and its `rg_main.c:1243` call site with it) —
  it was already dead weight before this proposal, unrelated to the overlay
  move. Small bonus cleanup, not part of the budget estimate above.
- **`clock_ensure_dirs()` is genuinely small and self-contained** — three
  `rg_storage_mkdir()` calls, no dependency on the rest of `rg_clock.c`
  (`rg_clock.c:2203`). This is the one real piece that has to stay resident.

## Proposed split

**Stays resident** (new tiny file, e.g. `Core/Src/retro-go/rg_clock_boot.c`,
or just leave the one function in place and don't move it):
- `clock_ensure_dirs()`

**Moves to `.overlay_clock`** (everything else):
- `rg_clock.c` minus `clock_ensure_dirs()` (includes `rg_clock_show()`, the
  new overlay entry point)
- `rg_clock_gif.c` minus the now-deleted `clock_gif_reserve()`
- `rg_clock_album.c` — confirmed self-contained: grepped every
  `clock_album_*` symbol tree-wide, only called from within `rg_clock.c`
  itself, nothing resident references it
- `rg_clock_alarm_mp3.c` — same confirmation, only called from `rg_clock.c`;
  also already partially overlay-based (it stages `.overlay_music` into
  RAM_EMU itself for MP3 decode, per `Makefile.common:1917` — proves the
  loader pattern this proposal wants is already proven inside this exact
  feature, just not for the feature's own UI/logic code)

## C++ global-ctor leak precedent (explicitly checked, per your instruction)

The C64/Frodo overlay bricked boot once because its C++ global statics
(`Prefs ThePrefs;`) emitted `.init_array` entries that fell into the
**resident** `.init_array` (no overlay `KEEP`), so `__libc_init_array()` ran
them at boot before the overlay was copied into RAM_EMU — jump into
not-yet-loaded memory, hard fault. Fix was the Lynx pattern: capture
`.init_array` inside the overlay's own linker block and run it manually via
`cpp_init_array()` after the overlay lands in RAM (see
`c64-frodo-overlay-ctor-bootcrash` project memory, and
`STM32H7B0VBTx_SDCARD.ld:1174-1176`).

**This risk class does not apply here.** All four `rg_clock*.c` files are
plain C (`.c`, compiled via `$(CC)`, not `$(CXX)`) — grepped for
`__attribute__((constructor))` (the only way plain C code emits `.init_array`
entries) across every file in the cluster: zero hits. There is no C++, no
global object with a constructor, nothing that can land in `.init_array`
regardless of overlay placement. The `.overlay_clock` block does **not** need
a `KEEP(.init_array*)` capture the way `.overlay_c64`/`.overlay_lynx` do — if
someone later adds C++ to this cluster, that's the trigger to revisit this,
not before.

## RAM_EMU concurrency (checked, wasn't asked but adjacent risk)

Confirmed via grep: `rg_clock_show()`/`rg_alarm_cache_due()` are referenced
**only** from `rg_main.c` (the launcher shell) — never from
`Core/Src/porting/<system>/` or the shared emulator frame-loop code in
`retro-go-stm32/components`. The alarm currently never fires while an
emulator core is loaded; it's only checked from the launcher's own menu idle
loop and at boot/wake. So `.overlay_clock` can safely share
`__RAM_EMU_START__` with every other overlay exactly like they already share
it with each other — nothing is ever resident in RAM_EMU at the same time as
the clock app today, and this proposal doesn't change that invariant.

## Loader mechanism (reuse, don't reinvent)

The exact "load `/cores/<x>.bin` from SD into RAM_EMU, clear BSS, jump to
`app_main_<x>`" machinery already exists and is table-driven:
`Core/Src/retro-go/rg_emulators.c:~1160-1240`, `emu_dispatch_t` +
`EMU_ENTRY()` macro, one row per core (e.g. `emu_vb`, `emu_lynx`). The plan
is to add a `.overlay_clock` block to `STM32H7B0VBTx_SDCARD.ld` (mirroring
`.overlay_gw`'s plain-C form, not `.overlay_c64`'s — no ctor capture needed
per above), add a `clock.bin` extraction line to `create_sd_data`/`flash_sd`
(mirroring the existing per-core `objcopy --only-section=.overlay_<x>`
lines), give `rg_clock_show()` an `EMU_ENTRY`-style signature, and route the
three `rg_main.c` call sites (`:579`, `:761`/`:896` menu paths, `:1266`
alarm-wake-at-boot path) through the same load-and-jump helper the emulator
dispatch table already uses instead of a direct function call.

## What's still open (why this stays a doc, not a commit)

- Whether `rg_clock_show()`'s current signature (`void(void)`, no args) can
  cleanly become an `EMU_ENTRY`-compatible entry point, or needs a thin
  resident trampoline that ignores the boot/rom-index args the dispatch table
  passes every core.
- Return path: cores exit back to the launcher through a known convention
  (menu re-entry after `app_main_<x>` returns) — need to confirm
  `rg_clock_show()`'s three call sites all tolerate becoming "load, run,
  return" instead of the current plain call, especially the boot-time
  `alarm_wake` path which runs before `emulators_init()`'s list rebuild that
  a comment at `rg_main.c:1264` says `rg_clock_show()`'s exit path depends on.
- Device-testable only once implemented: SD extraction + load-and-jump is new
  code, unlike the two already-landed fixes which were pure compile-flag /
  source-list changes. This one needs an actual boot-to-clock-and-back test
  on hardware before it ships, not just a link-size check.

Recommend implementing only after the two landed fixes (`d1dc577d`,
`2f36536b`) are confirmed on device, given this is materially more invasive
than either of those.
