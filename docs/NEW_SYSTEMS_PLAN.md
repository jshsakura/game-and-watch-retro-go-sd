# Plan: Three Final Systems — Odyssey², ZX Spectrum, Commodore 64

Status: **PLANNING ONLY** (not started). PCE-CD is wrapping up first.
Goal: add the last three feasible systems to the line-up. These are the systems
that fit BOTH ceilings of this device:

1. **Spec ceiling** — Cortex-M7 @ 280 MHz, `RAM_EMU` ≈ 724 KB (one core loaded at a
   time into `__RAM_EMU_START__`), 1–16 MB external QSPI flash. 16-bit full emu
   (SNES/GBA) is out; these three are 8-bit and comfortably inside budget.
2. **Button ceiling** — physical buttons are D-pad + A/B + GAME(=START)/TIME(=SELECT)/
   PAUSE only; no shoulders/X/Y. All three are joystick-era (D-pad + 1 fire), so they
   map cleanly. Keyboard-dependent titles use an on-screen overlay (precedent: MSX/CPC).

Why these three are the chosen "막타": CHIP-8 dropped (not a real console, toy
library, out of place). Arcade dropped (per-driver grind). Atari 800 dropped (user
decision).

---

## Shared integration pattern (applies to all three)

Established mechanisms confirmed in the codebase — reuse, do not invent:

- **Registration**: `Core/Src/retro-go/rg_emulators.c`
  - `#define MAX_EMULATORS 23` (line 415) → bump to **26**.
  - Add one `add_emulator(name, dirname, exts, RG_LOGO_PAD_*, RG_LOGO_HEADER_*, NO_GAME_DATA)`
    call in `emulators_init()` (lines 1599–1637) per system.
- **Core binary**: each core is a linker overlay section `.overlay_<sys>` in
  `STM32H7B0VBTx_{SDCARD,FLASH}.ld`, extracted to `/cores/<sys>.bin`
  (`Makefile.common` ~1556) and pushed to SD `/cores/` (~1661). Loaded into
  `__RAM_EMU_START__` at launch, then `app_main_<sys>(load_state, start_paused, save_slot)`.
  Dispatch added in `emulator_start()` keyed on `system_name`.
- **Build gate**: per-system `ROMS_<SYS>` conditional in `Makefile`/`Makefile.common`
  (see existing `ROMS_VIDEOPAC` at Makefile:21 / :713 as the template).
- **BIOS convention**: `/bios/<sys>/` on SD, user-supplied, git-ignored, cached to
  flash via `odroid_overlay_cache_file_in_flash()`. Precedent: PCE-CD
  (`main_pce.c:40`), MSX (`main_msx.c:867`), Coleco (`main_smsplusgx.c:127`).
  An open-BIOS bundled fallback is also precedented (PokeMini FreeBIOS,
  `forcefreebios`).
- **Input**: physical buttons → `ODROID_INPUT_*` (`odroid_input.c:48`) → core input
  in `main_<sys>.c`. D-pad + A as the single fire is the baseline for all three.
- **Icons**: RomM icon set → `RG_LOGO_PAD_<SYS>` / `RG_LOGO_HEADER_<SYS>`. Generate the
  28×28 colour pad icon + header art (same pipeline as Lynx icons).

All three core licenses are GPL-compatible with retro-go: O2EM Artistic-2.0
(already in tree), STECCY MIT / chips zlib, Frodo GPL-2.0.

---

## System 1 — Magnavox Odyssey² / Philips Videopac  (LOWEST EFFORT — do first)

The core is **already in the repo, just disabled**. This is mostly an
enable-and-finish job, not a port.

- **Core**: libretro **O2EM** submodule `external/o2em-go/` + glue
  `Core/Src/porting/videopac/main_videopac.c` (552 lines, full boot/run/blit/audio
  already implemented). License **Artistic-2.0**.
- **What's blocking it** (all in-tree):
  - Dispatch is double-disabled: `#if 0` at `rg_emulators.c:1422` **and** an
    undefined `ENABLE_EMULATOR_VIDEOPAC` macro → remove both gates.
  - Registration commented at `rg_emulators.c:1633` (misnamed "Philips Vectrex") →
    rename to **"Magnavox Odyssey²"**, dirname `videopac`, exts `bin lzma`.
  - **Graphics/icons** are the TODO ("change graphics", currently borrows
    `RG_LOGO_HEADER_AMSTRAD`) → make proper RomM pad + header art.
  - `Makefile:21 ROMS_VIDEOPAC :=` empty → wire into the default build.
- **BIOS**: `o2rom.bin` (exactly **1024 bytes**, CRC-checked; 4 known variants
  recognized). The port expects it **renamed to `bios.bin` in `/roms/videopac/`**
  (note: roms dir, not /bios — matches current `load_bios()` at
  `main_videopac.c:199`). User-supplied (copyrighted). Document in SD README.
- **Buttons**: D-pad → joystick, A → single Action button (code already collapses
  A+B to one fire). The ~48-key membrane keypad: a `vkeyb/*` overlay module is
  **already compiled but never wired into input** — wiring it to a function-button
  toggle is the optional secondary task (most action titles need only joystick+Action).
- **Gaps to note**: SaveState/LoadState are no-op stubs (`:62`) — add or accept "no
  save states" for v1. MegaCart support is `#if 0`'d.
- **Effort**: **Low.** Enable, rename, icons, BIOS doc, smoke-test. Save-states +
  vkeyb are follow-ups.

---

## System 2 — ZX Spectrum (Sinclair)  (BEST ROI)

- **Core (pick one)**:
  1. **STECCY** (`ukw100/STECCY`, **MIT**, C) — *recommended base*. Already bare-metal
     **STM32F407 @168 MHz** (weaker than our M7), does 48K **and** 128K, loads
     **.z80/.tap/.tzx** from FAT SD. Port = swap its display/audio/input/SD shims for
     retro-go's. Lowest risk.
  2. **floooh/chips** `zx.h`+`z80.h` (**zlib**) — cleanest dependency-free C core to
     drop into retro-go's module pattern; use **antirez/zx2040** (MIT, RP2040/264 KB)
     as the memory-trim reference. We have 724 KB so no need for its 128K cut.
- **Memory/perf**: 48K = 16 KB ROM + 48 KB RAM; 128K = 32 KB ROM + 128 KB RAM. Both
  fit easily. Target **48K first, then 128K**.
- **BIOS**: 16 KB (48K) / 32 KB (128K) Spectrum ROM. **Amstrad permits free
  distribution for emulation** (worldofspectrum.org/permits) → **legally bundleable**.
  Put in `/bios/zx/` (`48.rom`, `128.rom`).
- **ROM formats — easiest first**:
  - **.z80 / .sna** snapshots = direct state restore, no timing → **implement first.**
  - **.tap** = tape, flash-load via ROM-loader trap → moderate, second.
  - .tzx (exact pulse timing) / .trd,.scl (TR-DOS disk) → skip for v1.
  - dirname `zx`, exts `z80 sna tap` (+ `lzma`).
- **Buttons**: map D-pad + A → **Kempston** joystick (port 0x1F: R/L/D/U/fire) — the
  format the vast majority of action games use. Keyboard-only titles → on-screen
  overlay keyboard (reference STECCY's keymapping) as a secondary feature.
- **Effort**: **Low–Medium.** Z80 infra is familiar (MSX/CPC/SMS). New = display/
  audio glue + snapshot loader. Doing Spectrum first warms up the Z80 + `/bios/` +
  SD-stream + overlay-keyboard patterns that C64 reuses.

---

## System 3 — Commodore 64  (HEAVIEST — do last, as the "본편")

- **Core**: **Frodo** (`cebix/frodo4`, classic V4.1b lineage, **GPL-2.0**) in **"PC"
  (line-based) mode**. Reference MCU port: **Schuemi/c64-go** (ESP32 @240 MHz/520 KB —
  weaker than us). Built-in **lightweight integer SID** (not reSID — exactly what an
  MCU needs). Full `.prg`/`.crt`/`.d64`/`.t64` + virtual-1541 (fast SD-directory
  access, fits our SD-stream pattern).
  - **"SC" (single-cycle, cycle-exact) mode is the stretch goal** — c64-go reports it's
    too slow at 240 MHz; assume PC mode for full speed and accept some
    compatibility/demo gaps.
  - Cross-check: **MCUME/c64pico** runs C64 on an RP2040 (264 KB, ≤250 MHz) — proof it
    fits a smaller MCU than ours.
- **Memory/perf**: 64 KB RAM + color RAM + 20 KB ROMs + state ≈ <120 KB → trivial in
  724 KB. The cost is **VIC-II raster + SID CPU time**, not RAM. The device already
  runs Genesis 68000, so PC-mode C64 should hit full speed.
- **BIOS**: KERNAL 8 KB + BASIC 8 KB + CHARGEN 4 KB = 20 KB. Copyrighted (Cloanto) →
  **user-supplied in `/bios/c64/`** (matches c64-go's `/roms/c64/bios/`). Legal
  bundled fallback = **MEGA65/open-roms** (GPL-3.0, boots + IEC load, but not 100%
  game-compatible) — ship as optional fallback like PokeMini FreeBIOS.
- **ROM loading — easiest first**: **.prg direct-inject** and **.crt cartridge** first
  (instant, no 1541). Then **.d64** via Frodo's virtual-1541 fast path. Avoid true
  cycle-exact 1541 drive emulation (second 6502 = heavy).
  - dirname `c64`, exts `prg crt d64 t64` (+ `lzma`).
- **Buttons**: D-pad + A → joystick port 2 + fire (baseline ◎). **Keyboard dependency
  is larger than Spectrum** (Space/Return/F1 start keys) → reuse the overlay keyboard
  built for Spectrum/Odyssey². CPC/MSX precedent means this UX is already accepted.
- **Effort**: **Medium–High.** Make-or-break = (1) trim Frodo's SDL/GTK frontend to
  the bare core, (2) PC-mode + integer SID at full speed, (3) .prg/.crt loader.
  Reuse everything proved out by the Spectrum port.

---

## Suggested order & rationale

**Odyssey² → ZX Spectrum → C64** (effort ascending; each unlocks the next):

1. **Odyssey²** — nearly free (enable in-tree core + icons + BIOS doc). Quick win,
   validates the add_emulator/overlay/`/cores` flow end-to-end on a new tab.
2. **ZX Spectrum** — establishes Z80-reuse + `/bios/` bundling + snapshot loader +
   overlay keyboard. Legal BIOS bundle = cleanest of the three.
3. **C64** — the marquee system; reuses Spectrum's overlay keyboard + SD-stream +
   `/bios/` patterns, so the only net-new risk is Frodo core trimming + SID perf.

## Open decisions (resolve at kickoff, per system)
- Save-state support in v1? (Odyssey² currently stubbed; Spectrum/C64 snapshots make
  it natural.)
- Bundle open BIOS fallbacks (Spectrum legal-bundle yes; C64 open-roms optional;
  Odyssey² user-supplied only)?
- Overlay keyboard: build once, shared across Spectrum/C64/Odyssey²-keypad.
- Flash budget: confirm three more `/cores/*.bin` fit the SD-content + GIT_TAG-locked
  core scheme (see Lynx core-lock infra).

## Source references
- Odyssey²: O2EM https://github.com/libretro/libretro-o2em (in-tree `external/o2em-go`)
- Spectrum: STECCY https://github.com/ukw100/STECCY · chips https://github.com/floooh/chips ·
  zx2040 https://github.com/antirez/zx2040 · ROM permits https://worldofspectrum.org/permits
- C64: frodo4 https://github.com/cebix/frodo4 · c64-go https://github.com/Schuemi/c64-go ·
  c64pico https://github.com/silvervest/c64pico · open-roms https://github.com/MEGA65/open-roms
