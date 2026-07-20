# Internal flash budget — where the 256 KB goes, and how to get it back

**Question this answers:** the canonical Docker release build (`make release DOCKER=1
COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1
INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1`) links to
**262,100 / 262,144 bytes — 44 bytes free**. Does a feature have to go?

**Verdict: no.** Two narrowly-scoped, zero-feature-loss fixes recover **~5.7 KB**
(130× today's margin) from a single already-fixed-elsewhere bug class. A third,
larger structural fix (~20 KB) is available if the margin ever gets tight again.
None of them touch coverflow, cheat codes, or any locale — and the investigation
below shows *why* dropping a locale wouldn't have helped anyway.

All numbers below come from parsing `build/gw_retro_go.map` from the build that
produced the 262,100-byte binary (`build/gw_retro_go_intflash.bin`, timestamped
today). No build was run to produce this document.

## Method: how 262,100 is actually assembled

`Makefile:1510` builds the intflash image with:

```
objcopy -j .isr_vector -j .firmware_abi -j .text -j .rodata -j .ARM.extab \
        -j .preinit_array -j .init_array -j .fini_array -j .data -O binary
```

`objcopy -O binary` doesn't just concatenate the named sections — it lays them out
by **load address** and zero-fills any address gap between them. That matters:
`.ARM` (the `.ARM.exidx` unwind-index table) sits in the linker script *between*
`.ARM.extab` and `.init_array`, both of which **are** copied — but `.ARM.exidx`
itself is **not** in the `-j` list. Its address span still has to be crossed, so
every byte of it becomes zero-padding in the shipped image. Confirmed by address
arithmetic: `.data` load address + size − `.isr_vector` start = `0x3ffd4` =
**262,100**, exactly the reported figure — i.e. the reported budget already
includes this dead padding.

## Where the 262,100 bytes are, top-down

| Region | Bytes | % | What |
|---|---:|---:|---|
| `.text` | 207,496 | 79.2% | Resident code |
| `.rodata` | 49,256 | 18.8% | Resident constants/tables |
| `.ARM` (exidx, **unused padding**) | 1,704 | 0.65% | See below — not copied, still costs space |
| `.isr_vector` / `.firmware_abi` | 1,208 | 0.46% | Boot vector table + firmware ABI header — fixed cost |
| `.ARM.extab` | 344 | 0.13% | C++ exception tables — see below |
| `.data` (load size) | 1,740 | 0.66% | Initialized globals' flash-resident copy |
| `.init_array`/`.fini_array` | 8 | ~0 | ctor/dtor pointer tables |

`.text` + `.rodata` = 98% of the budget, so that's where the real levers are.

### Top resident `.text` consumers (object file, bytes)

| Object | Bytes | Category |
|---|---:|---|
| `rg_clock.c` + `rg_clock_gif.c` + `rg_clock_album.c` + `rg_clock_alarm_mp3.c` | 19,504 | ② movable |
| `fatfs/ff.o` (FatFS core) | 14,336 | ① must stay |
| ST HAL drivers (`hal_dma`, `hal_rcc_ex`, `hal_ospi`, `hal_adc`, `hal_jpeg`, `hal_rcc`, `hal_sai`, `hal_msp`, `hal_spi`, …) | ~28,382 (partial — more not in top 30) | ① mostly, ④ maybe |
| `gui.o` (launcher menu) | 10,494 | ① must stay |
| `odroid_overlay.o` (dialogs/toasts, includes `font8x8_basic` 1,024 B) | 9,376 | ① must stay |
| `rg_emulators.o` (ROM/core catalogue) | 9,096 | ① must stay |
| `LzmaDec.o` (LZMA decompressor) | 5,712 | needs a caller audit — see open item |
| `main.o` | 3,770 | ① |
| `user_diskio_spi.o` | 3,724 | ① SD driver |
| `common.o` | 3,402 | ① |
| `rg_i18n.o` | 3,316 | ① — see "locale flags" below |
| newlib `strtod`+`mprec` chain | ~5,694 | ③/Tier-2 — see leak section |
| newlib `libm` (`pow`,`log`,`exp`,`sin`,`cos`,`atan`,`floor`,`sqrt`) | ~15,000+ | mixed — see leak section |

### Top resident `.rodata` consumers

| Object / symbol | Bytes | Note |
|---|---:|---|
| merged string-literal pool (misattributed to `lz4_depack.o` by the linker) | 13,976 | **Not** lz4-specific — this is every deduplicated format/log/path string in the resident image, coalesced by `-fmerge-all-constants`. Real content, hard to shrink without editing individual strings. |
| `rg_logos.o` (cover-flow icons, ~30 × `cicon_<system>_data`, ~200–300 B each) | 10,188 | ① — needed for the icon each system shows in the launcher/coverflow |
| `libm_a-pow_log_data.o` / `log_data.o` / `exp_data.o` (lookup tables backing `pow`/`log`/`exp`) | 4,168 + 2,192 + 2,160 = 8,520 | Tier-2 leak, see below |
| `rg_main.o` (`.intflash_global`, emulator metadata table) | 3,676 | ① |
| `rg_i18n.o` (`lang_metadata` 288 B + `lang_en_us` 1,132 B — **only the English fallback table is compiled in**) | 1,452 | ① — see locale note |
| `odroid_overlay.o` (`font8x8_basic`) | 1,024 | ① — boot/rescue-screen font |
| `crc32.o` | 1,024 | ① |
| C64/Frodo small metadata (`C64.o`, `1541d64.o`, etc.) | ~1,276 | see Fix #1 |

## Fix #1 (do this first): C64/Frodo is the only core leaking C++ unwind machinery into flash — ~5.7 KB, zero risk

`build/gw_retro_go.map`'s resident `.ARM.extab` and `.ARM` (exidx) sections were
audited entry-by-entry. **Every real byte in both comes from `build/c64/*.o`**
(Frodo, the C64 core) plus two libgcc runtime files that exist *only* to service
those tables:

```
$ awk '/^\.ARM  .*0x0813f258/{f=1} /^\.rel\.dyn/{f=0} f' build/gw_retro_go.map \
  | grep -oE "build/[a-z0-9_]+/|libgcc\.a\([a-z-]+\.o\)" | sort -u
build/c64/
libgcc.a(pr-support.o)
libgcc.a(unwind-arm.o)
```

`unwind-arm.o` + `pr-support.o` (the actual ARM EH personality-routine runtime,
not just index tables) contribute **3,612 bytes of real `.text`** — pulled in
*solely* because C64's C++ translation units emit exception tables that
reference them.

This is the exact bug class already fixed for three other C++ overlay cores.
`STM32H7B0VBTx_SDCARD.ld` shows `a2600`, `lynx`, and `tgbdual` all explicitly
route their unwind fragments into their own RAM_EMU overlay:

```
# a2600 (line 664), lynx (693), tgbdual (292):
build/a2600/*.o (.data .data* .text .text* .rodata .rodata* .ARM.extab.text.* .ARM.exidx.text.*)
```

C64's overlay block (line 1176) never got the same treatment:

```
build/c64/*.o (.data .data* .text .text* .rodata .rodata*)
                                            # ^ missing .ARM.extab.text.* .ARM.exidx.text.*
```

This is also *not* the same bug as the already-documented and already-fixed
C64 `.init_array` ctor crash (`c64-frodo-overlay-ctor-bootcrash` in project
memory) — that one was about constructors running before the overlay was
loaded; this one is purely about where the exception-unwind metadata lands.

**Two ways to fix it — pick one:**

- **(a) Match the existing pattern** — append `.ARM.extab.text.* .ARM.exidx.text.*`
  to C64's `build/c64/*.o (...)` capture at `STM32H7B0VBTx_SDCARD.ld:1176`, same as
  a2600/lynx/tgbdual. Moves the C64-attributable extab/exidx content into the
  RAM_EMU overlay, where C64 already has (comparatively) much more headroom.
- **(b) Simpler and more complete** — add `-fno-exceptions -fno-unwind-tables
  -fno-asynchronous-unwind-tables` to the C64 C++ compile rule
  (`Makefile.common:1176`, the `$(BUILD_DIR)/c64/%.o: %.cpp` recipe), matching
  what `Core/Src`'s own C++ files already use one rule below
  (`Makefile.common:1215`) and what VB/GBA/pico8 already do (`-fno-exceptions`).
  No `throw`/`catch` exists anywhere in `Core/Src/porting/c64/frodo/` (grepped,
  zero hits), so this changes no behavior. This option also removes the need
  for `unwind-arm.o`/`pr-support.o` entirely (nothing left to reference them),
  recovering the full **~5,660 bytes** (344 extab + 1,704 exidx padding + 3,612
  unwind runtime) in one line, resident and overlay both.

Recommend **(b)** — it's a one-line Makefile change, matches an existing,
already-proven flag combination used elsewhere in this exact codebase, and
recovers more than (a) because it also drops the runtime.

**Before landing:** confirm no other C64 code path expects a thrown
`std::bad_alloc` or similar to be caught (grep says no `catch` anywhere in the
core), then verify with `make docker` that the intflash size drops by roughly
5.6 KB and C64 still boots/saves/loads on device.

**Update — commits landed (`d1dc577d`, `2f36536b`), host-level regression audit done:**

- `throw`/`catch`/`dynamic_cast`/`typeid` grepped across every file in
  `Core/Src/porting/c64/` and `Core/Inc/porting/c64/`, headers included (the
  original check only covered `.c`/`.cpp`, not headers) — zero hits. Since
  `-fno-exceptions` turns any `throw`/`try`/`catch` into a hard **compile
  error** (not a silent behavior change), this also means the ARM build
  cannot fail to compile from this change — there's nothing for the flag to
  reject.
- `new`/`new[]`/`delete`/`delete[]` are globally overridden
  (`Core/Src/heap.cpp:43,49,55,62`) to route through a custom bump allocator
  (`heap_alloc_mem()`) that signals OOM via a plain C `assert()`, not the
  standard-mandated `throw std::bad_alloc`. This override is global (applies
  to the whole firmware, not just C64), so C64's several `new`/`new[]` call
  sites (`C64.cpp`, `IEC.cpp`, `SID.cpp`, `1541d64.cpp`, `1541t64.cpp`,
  `main_c64_dev.cpp`) never relied on real exception-throwing `new` in the
  first place — `-fno-exceptions` changes nothing about their OOM behavior.
- No STL headers (`<vector>`, `<string>`, `<map>`, etc.) included anywhere in
  the C64 tree — rules out libstdc++-internal throw paths hiding inside
  template code that wouldn't show up in a plain grep for `throw`.
  No `__builtin_return_address`/backtrace usage either.
- The existing `.init_array` ctor-overlay fix
  (`STM32H7B0VBTx_SDCARD.ld:1174-1176`, `c64-frodo-overlay-ctor-bootcrash`) is
  untouched by this change — constructor ordering and exception-unwind tables
  are orthogonal ABI mechanisms; nothing here reopens that bug.

**Net: host-level static audit found nothing that could regress.** The only
way this specific flag change could break anything is a compile error, which
would be caught immediately and loudly by the next build — not a silent
runtime regression. What a host audit *can't* substitute for: confirm on
device that C64 still boots, loads a ROM, and does one save/load round-trip
— not because anything above suggests it won't, but because that's the
cheapest way to close the loop on "did the build actually succeed" for a
change to a whole translation unit's compile flags.

## Fix #2 (small, same root cause, worth doing while you're in there): `lz4_depack.c` is resident but has exactly one caller, and it's an overlay

`Core/Src/porting/lib/lz4_depack.c` is listed directly in the top-level
`Makefile`'s resident `C_SOURCES` (`Makefile:28`), so it's always compiled into
`build/core/lz4_depack.o` regardless of which cores are enabled. Its only real
caller anywhere in the tree:

```
external/LCD-Game-Emulator/src/gw_sys/gw_romloader.c:175:
    rom_size_src = lz4_uncompress(src, dest);
```

— the game.com core (`.overlay_gw`), and nothing else (checked
`retro-go-stm32/` and `Core/` broadly; `SD_CARD=1`, this repo's default, is
documented to *omit* ROM compression for the resident loader path, so the
launcher itself has no reason to call it). Real code cost is small — `lz4_depack`
(0xb0) + `lz4_uncompress` (0x6c) + `lz4_get_file_size` (0x48) = **356 bytes** —
but it's mis-scoped the same way C64's unwind tables were: single-consumer code
living in the shared resident budget instead of that consumer's overlay. Move
the source into game.com's own object list (or route it through the same
`build/gamecom/*.o (...)` overlay capture) and it stops costing intflash at all.
Low value on its own (356 B), but zero risk and same category of fix as #1, so
bundle it into the same PR.

## Fix #3 (bigger, more effort, hold in reserve): the clock app is architecturally identical to an emulator core and could be an overlay

`rg_clock.c` + `rg_clock_gif.c` + `rg_clock_album.c` + `rg_clock_alarm_mp3.c`
together cost **19,504 B of `.text` + 677 B of `.rodata` = 20,181 bytes — 7.7%
of the entire intflash budget** — for a launcher *app* (clock, alarms, themes,
GIF backgrounds, photo album), not core boot/driver functionality. It is never
running concurrently with an emulator core, exactly the usage pattern every
`.overlay_<system>` core already exploits (loaded into `RAM_EMU` only while
active). In fact the clock app **already does this for its own MP3 alarm
decoder** — `Makefile.common:1917` stages `.overlay_music` into `RAM_EMU` "by
rg_clock_alarm_mp3.c" — so the loader plumbing pattern is proven in this exact
feature, just not applied to the clock's own UI/logic code.

This is not a quick fix (needs a `.overlay_clock` linker block, an SD-staged
`clock.bin` the same way `create_sd_data` extracts cores, and a menu entry that
loads it before running it, mirroring `flash_sd`'s core-extraction pattern) but
it is a **zero-feature-loss** ~20 KB lever — the single largest one found — and
should be reached for before any suggestion to cut coverflow/cheats/locales.

## Tier-2 finding, not yet actionable without more verification: several overlay cores' library dependencies (libm, strtod) are leaking into intflash the same way C64's unwind tables were

Cross-referencing the map's "Archive member included ... by file (symbol)"
section shows these newlib functions are pulled into the **shared resident**
image, but every one of them is needed by exactly one **overlay** core, not by
the launcher itself:

| libm/libc symbol | Pulled in by | Bytes (func + data table) |
|---|---|---:|
| `atof` → `strtod`+`mprec` chain | `build/vb/vb_set.o` (Virtual Boy INI config) | ~5,694 |
| `sin`, `log` | `build/nes_fceu/fceu-emu2413.o` (YM2413 FM synth) | ~508 + 2,712 |
| `atan`, `cos` | `build/nes_fceu/nsf.o` (NES NSF music player) | ~532 + 508 |
| `exp` | `build/amstrad/psg.o` (Amstrad PSG sound) | ~2,588 |
| `sqrt` | `build/md32x/pico__sound__resampler.o` (32X audio resampler) | small, not yet measured |

`pow` (the single biggest libm consumer, ~5,672 B) is **not** part of this list
— it's called by `build/core/gw_firmware_abi.o`, which is genuinely resident.
Leave it alone.

Grepped `retro-go-stm32/components/` and all non-overlay `Core/Src/` for any of
`sin/cos/atan/log/exp/sqrt/atof` — zero hits. Nothing resident needs any of
these five.

**Why this happens:** every existing overlay-capture rule in the linker script
(`build/<core>/*.o (...)`) only matches *object files compiled from that core's
own sources*. A static-library archive member (`libm.a(libm_a-s_sin.o)`, etc.)
pulled in transitively by one of those objects doesn't match the wildcard, so
it falls through to the generic `*(.text)` / `*(.rodata)` catch-all — which is
the resident FLASH section. Same root cause as Fix #1, just against libm/libc
instead of libgcc's unwind runtime.

**Why this is Tier 2, not a fourth ready-to-ship fix:** GNU ld does support
`archive:member(sections)` syntax to route a specific library object into an
overlay's output section (e.g. `*libm.a:libm_a-s_sin.o(.text* .rodata*)`
inside `.overlay_nes_fceu`), so the fix is mechanically the same idea as #1 —
but it touches four different overlay blocks, and needs a real build
(`make docker`) to confirm nothing else transitively needs the same archive
member before it's excluded from the resident catch-all. Rough combined upside
if all five are moved: **~14–16 KB**. Worth a follow-up pass, not blocking.

## What was ruled out, with numbers

- **Dropping a locale (ZH_CN/ZH_TW/KO_KR/JA_JP/etc.) saves almost nothing.**
  Font glyph bitmaps for every language live in `sd_content/fonts/` and are
  loaded from the SD card at runtime — they were never compiled into intflash
  in the first place. Every enabled locale's *string table* is also loaded
  from SD at runtime (`i18n_load_language`, `get_font_data` — confirmed by
  reading `rg_i18n.o`'s rodata: only `lang_en_us`, the built-in fallback, is
  actually embedded, at 1,132 B). What's compiled in per locale is a ~20–40
  byte date-format stub function (`ko_kr_fmt_Title_Date_Format`, etc.) — twelve
  of them exist in this build (`de_de` through `zh_tw`) totaling well under
  500 bytes combined. Removing Korean/Chinese/Japanese support would free
  roughly **one locale's worth of stub, ~30–40 bytes** — not worth the user
  cost.
- **Dead code isn't the problem.** `-ffunction-sections -fdata-sections` (build)
  and `--gc-sections` (link) are already active (`Makefile.common:764,816`), so
  everything counted above is code the linker proved is reachable.
- **The optimization level isn't the problem.** The top-level `Makefile:5`
  unconditionally sets `OPT = -O2 -ggdb3`, overriding `Makefile.common`'s
  `-Og` default — this build is already at `-O2`, not an unoptimized debug
  level. Switching the *launcher's own* sources (not the overlay cores, which
  already have per-core `-Os`/`-O2`/`-O3` tuning) to `-Os` could still shave a
  further 5–15% off `.text` (a well-known GCC size/speed tradeoff), and is
  low-risk because none of the launcher's own resident code is a
  frame-rate-critical hot path — only the overlay cores are, and they're
  unaffected. Flagged as a possible Tier-3 lever, not measured here (would
  require an actual `-Os` build to quantify).
- **The 13,976-byte "lz4" rodata blob is not lz4's fault.** It's the whole
  program's merged string-literal pool, mislabeled by the linker because
  `-fmerge-all-constants` coalesces every translation unit's string constants
  into sections named after whichever one linked first alphabetically. Real
  content (log/format/path/assert strings across the whole resident image);
  shrinking it means editing individual strings, not moving a module.

## Recommended order of operations

1. **Fix #1** (C64 unwind flags, `Makefile.common:1176`) — one line, ~5.6 KB,
   zero behavior change, matches an existing pattern used three times already
   in this codebase.
2. **Fix #2** (move `lz4_depack.c` into game.com's own build) — small (356 B)
   but same PR, same justification.
3. Rebuild with the canonical release flag set, confirm the map file shows
   `build/c64/` gone from `.ARM.extab`/`.ARM`, confirm intflash usage dropped
   by ~5.6 KB combined, confirm C64 boots + saves/loads on device (unwind flag
   change touches its whole C++ compile unit).
4. Hold **Fix #3** (clock → overlay) and the **Tier-2 libm/strtod overlay
   capture** in reserve — don't build them speculatively. With #1+#2 landed the
   margin goes from 44 B to ~5.7 KB (130×), which should hold through normal
   development for a while. Revisit #3 the next time margin gets tight before
   ever proposing to cut a feature.
