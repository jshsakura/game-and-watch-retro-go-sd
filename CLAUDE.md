# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

A multi-emulator launcher for the Nintendo Game & Watch (STM32H7B0VB MCU). This is the SD-card variant of [sylverb/game-and-watch-retro-go](https://github.com/sylverb/game-and-watch-retro-go-sd) — emulator cores and ROMs live on a microSD card rather than being baked into the external flash. Each cartridge format (NES, GB, MSX, Genesis, etc.) has its own emulator core ported to STM32 with very tight memory and CPU constraints.

## Build / flash workflow

The build system is plain GNU Make. `Makefile` lists source files; `Makefile.common` contains the rules, toolchain setup, and configuration variables.

**Toolchain.** Requires `arm-none-eabi-gcc` v10+ (CI/Docker uses 15.2.rel1). Either put the toolchain on `PATH` or set `GCC_PATH=/path/to/bin` on the make command line. `gnwmanager` (Python, from `requirements.txt`) is required for any flashing target — install via `python3 -m pip install -r requirements.txt`.

**Common targets** (run from repo root, all support `-j$(nproc)`):

- `make docker` — is the proper way to check if project builds and links. If docker is not available, use `make -j8 CHECK_DIRTY_SUBMODULE=0 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 release` to use locally installed compilation environment.

- `make help` — full list of build flags with current values.

**A release MUST use the canonical flag set.** `make release DOCKER=1` on its own leaves every
feature flag at its default of `0` — three builds shipped with no coverflow, no cheat codes, no
overclock and no non-Latin locales (so the launcher could not even offer Korean). The set that
CI uses lives in `.github/workflows/package.yml` and is the only correct one:

```
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
             ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

There are no automated tests. Verification is manual: build, flash, run on hardware —
but see "Testing a core the way the device runs it" below before trusting a host harness.

**Configuration knobs that change layout (not just behavior)** — pass on the make command line:

- `GNW_TARGET={mario,zelda}` — different button mappings and default extflash size.
- `INTFLASH_BANK={1,2}` (or `INTFLASH_ADDRESS=0x08...`) — selects which 128k/256k internal-flash bank the code is linked into. Bank 2 is used with dual-boot OFW patches.
- `SD_CARD=1` (default for this repo) — enables FatFS, omits ROM compression, and uses `STM32H7B0VBTx_SDCARD.ld`. Setting `SD_CARD=0` switches to the all-in-flash variant (different link script, different feature set).
- `EXTFLASH_SIZE_MB`, `EXTFLASH_OFFSET`, `LARGE_FLASH` — external flash sizing (deprecated in favor of `EXTFLASH_SIZE_MB`).
- `CHEAT_CODES=1`, `COVERFLOW=1`, `SHARED_HIBERNATE_SAVESTATE=1`, `DISABLE_SPLASH_SCREEN=1`, `MSX_USE_BANK_2=1`, `FORCE_NOFRENDO=1` — feature toggles. The Docker release build enables `COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 INTFLASH_BANK=2 CHEAT_CODES=1`.
- `CODEPAGE`, `UICODEPAGE`, individual locale flags (`FR_FR`, `RU_RU`, …) — controls font/i18n inclusion.

**Submodule hygiene.** `external/` holds each emulator core as a git submodule. The build refuses to run if submodules are dirty or out of sync — fix with `git submodule update --init --recursive`, or pass `CHECK_DIRTY_SUBMODULE=0` to bypass (the Docker target does this).

## Architecture

### Three storage tiers, one ELF

A single ELF (`build/gw_retro_go.elf`) is partitioned by linker sections into three physical destinations:

1. **Internal flash** (`*_intflash.bin`, from sections `.isr_vector .text .rodata .data .init_array …`) — launcher UI, drivers, FatFS, the always-resident retro-go shell. Linker script: `STM32H7B0VBTx_SDCARD.ld` (or `_FLASH.ld` for non-SD builds).
2. **External flash** (`*_extflash.bin`, sections `._extflash ._itcram_hot ._ram_exec` + every `.overlay_*`) — on SD-card builds this is mostly used during build/test; at runtime cores are streamed from SD instead.
3. **SD card** — `make create_sd_data` extracts each `.overlay_<system>` section into a standalone `cores/<system>.bin` file with `objcopy --only-section`. The launcher loads exactly one core at a time into `RAM_EMU` — every `.overlay_<system>` is linked at the same VMA, `__RAM_EMU_START__` — before running it. Each emulator therefore gets that whole region to itself but cannot coexist with another core in memory.

   **`RAM_EMU` is 724 KB (741,376 bytes)**, not "about a megabyte": `__RAM_EMU_LENGTH__ = 1024K - 300K`, the 300 KB being the LCD framebuffer pool (`STM32H7B0VBTx_SDCARD.ld:73-74`). A core's `.overlay_<name>` (text+rodata+data) **and** its `.overlay_<name>_bss` must both fit inside it; the linker `ASSERT`s it. The heaviest cores already use 94–99.8% of it (MUSIC 722.9 KB, ZELDA3 695.7 KB, SM 682.6 KB, MD 681.2 KB — from `build/gw_retro_go.map`), so treat the region as full, not roomy. A core can reach for more at runtime — `itc_malloc()` (64 KB ITCM), `ahb_malloc()` (120 KB AHB SRAM) — but the DTCM heap is shared with the launcher and is tight.

Adding a new emulator means: add its sources to `Makefile`, give it a `.overlay_<name>` section in the linker script, and add a `--only-section=.overlay_<name>` extraction line to `create_sd_data` plus an `sdpush` line in `flash_sd`.

### Source tree layout

- `Core/Src/main.c`, `Core/Src/gw_*.c` — STM32 HAL bring-up, LCD, audio (SAI), buttons, SD driver, RTC, battery (BQ24072), flash chip access, low-level memory allocator. Headers in `Core/Inc/`.
- `Core/Src/porting/<system>/main_<system>.c` — the per-emulator porting layer. This is where most emulator-specific Game & Watch work happens: input mapping, video scaling, audio bridging, savestate hooks, ROM loading, options menus.
- `Core/Src/porting/lib/` — shared helpers used by porting code: FatFs vendor copy, LZ4/LZMA decompressors, HW JPEG decoder, HW SHA1, softspi.
- `Core/Src/porting/odroid_*.c` — the retro-go shell's portability glue (input, display, audio, overlay, sdcard, system). Names come from the original Odroid-GO Retro-Go.
- `retro-go-stm32/` — vendored snapshot of upstream retro-go (launcher UI, settings, common emulator-side helpers in `components/odroid/`, plus three emulator cores `gnuboy-go`, `nofrendo-go`, `pce-go`, `smsplusgx-go`).
- `external/` — git submodules for every other emulator core (`fceumm-go`, `blueMSX-go`, `caprice32-go`, `gwenesis`, `LCD-Game-Emulator`, `stella2014-go`, `prosystem-go`, `PokeMini-go`, `potator`, `tamalib`, `tgbdual-go`, `ccleste-go`, `zelda3`, `smw`, `o2em-go`, `firmware_update`). Each is a third-party emulator/port with its own license; we patch them via `genpatch.py`-managed `.patch` files where present.
- `tools/` — Python utilities. The user-facing ones (per README):
  - `gencovers.py` — generate `.img` cover thumbnails for ROMs (uses `requirements.txt`).
  - `fonttool/`, `png_to_logo.py`, `img2bin.py`, `pllgen.py`, `gen_fceu_palettes_table.py` — asset converters used by the build.
- `scripts/` — shell helpers invoked from the Makefile (size reporting, git tag stamping, release packaging, rom discovery). Run from the repo root.
- `assets/`, `icons/`, `smw_redefines`, `zelda3_redefines` — graphics, system icons, SNES symbol-rename headers for the homebrew SNES ports.

### Adding/modifying a core

Emulator main loop happens in `Core/Src/porting/<system>/`, a loop iteration should run the generation of a frame and to write it in the framebuffer, and to generate the audio samples for the frame. The submodule under `external/<system>` contains machine emulation logic.

The `retro-go-stm32/components/odroid/` API (`odroid_system`, `odroid_overlay`, `odroid_display`, `odroid_input`, `odroid_audio`, `odroid_sdcard`, `odroid_netplay`) is the contract between the launcher and an emulator core. New cores implement against it.

## Testing a core the way the device runs it

A host harness that compiles *the core's whole source tree* is not testing the firmware. Three
Super Metroid releases shipped that could not boot while the harness reported 4,000 frames with
zero mismatches against the reference emulator, because the harness was a different program:

- It compiled `sm_cpu_infra.c`, which the firmware excludes — and which **defines and sets
  `g_snes`**, the pointer sm's whole register bus goes through. On the device nothing set it.
- It compiled without `TARGET_GNW`, so it built itself a **real SPC700**. The device has
  `snes->apu == NULL` (spc_player is the sound chip), and the runtime dereferenced it.

Both are device-only faults that a host build simply cannot reach.

Then a fourth release booted straight into a Hardfault anyway, because the same program on a
different CPU is still not the same program:

- **The host does not trap what ARM traps.** `ClearBackdrop()` (sm's `ppu.c`) fills a `uint16`
  buffer through a `*(uint64*)` cast; on ARM that is **`STRD`, which faults unless the address is
  word-aligned**, and the buffer sat at offset `0x702` inside `Ppu` — 2 mod 4. The device died on
  the first rendered line; x86 and aarch64 store unaligned without complaint and rendered 4,000
  happy frames. Note Cortex-M7 traps *only* 64-bit accesses this way — unaligned halfword/word
  accesses are legal, and the SNES code does them constantly, so only the 64-bit ones matter.
- **An implicit declaration is a lie that only 64-bit hosts catch.** `spc_player.c` called
  `ahb_malloc()` with no prototype, so it returned `int`. On the 32-bit device the truncated
  pointer is still the pointer; on a 64-bit host it is a wild address, and the harness died in
  `SpcPlayer_Create` before reaching any emulation at all.

Three things now close the gap, and a new core should copy all three:

- **`tools/sm_harness/device_run.sh`** — compiles the core from the Makefile's own source list
  (never a copy of it), with the device's defines (`-DTARGET_GNW`), and shims the firmware
  allocators. It also forces the device's *CPU* rules on the host: `-fsanitize=alignment` (failing
  only on the 64-bit violations, which is exactly what an M7 traps) and
  `-Werror=implicit-function-declaration`. Revert any of the fixes above and it reproduces the
  fault on the host. Give it a ROM path and it runs and gates: `device_run.sh <rom> [frames]`.
- **`tools/sm_harness/device_parity.sh`** (in `tests/run.sh`) — links exactly what the device
  links. Anything left undefined is a symbol the firmware linker would resolve *silently* to
  another core's.
- **A `_Static_assert` next to any type-punned store**, so an alignment assumption the code makes
  is one the compiler has to prove rather than one the struct layout grants by luck.

## Cores are overlays — a missing symbol does not fail the link, it aliases

Every emulator core is an overlay linked at the same RAM address (`__RAM_EMU_START__`). If core A
references a global that only core B defines, **the linker binds it, quietly**, to B's address —
which, once A is loaded, holds A's own unrelated data. Super Metroid drove the SNES bus through
Super Mario World's `g_snes` for three releases and asserted on the first register read.

A core's globals must be renamed into its own namespace by its `<core>_redefines` file, so that a
missing definition is a **link error** instead of an alias. `scripts/check_core_symbol_aliases.py`
runs on every link and fails the build if any core reaches a symbol another core's overlay owns
(it confirms by disassembly, so dead references do not trip it).

## Adding an APPID resets every user's settings

`/CONFIG` is a raw dump of `persistent_config_t`, and that struct contains `app[APPID_COUNT]`. Add
an entry to `appid.h` and the struct grows, the saved file no longer matches, and the magic check
throws it away — language, coverflow, backlight and volume all go back to defaults. The `version`
field exists to make that deliberate. Do not add an APPID casually.

## Things that are easy to get wrong

- **Don't edit files under submodules in `external/`.** Either fix upstream or add/update a patch. The build's submodule-dirty check will reject the build.
- **`retro-go-stm32/components/odroid/*.c` is a mostly-unused ESP32 vendor snapshot with the same filenames as the real STM32 implementation, e.g. `odroid_audio.c` (I2S, `AUDIO_SAMPLE_RATE=48000`, `i2s_driver_install`) vs. the actually-compiled `Core/Src/porting/odroid_audio.c` (SAI DMA, real hardware sample rate set per-app via `odroid_system_init`'s second argument, reconfigures PLL2 through `set_audio_frequency()`). Check `Makefile`'s `C_SOURCES`/per-system source lists for which file actually links before trusting anything you read under `retro-go-stm32/components/odroid/` — several of the `.h` files there declare wider function signatures (e.g. `odroid_system_emu_init` with 6 params) than the vendor `.c` implements (3 params), another sign it's stale reference material, not what runs. The header is still live (included via `<odroid_system.h>`); only some of the paired `.c` files are.
- **`SD_CARD=1` and `SD_CARD=0` are very different builds.** Different linker scripts, different feature set, different binary layout. Default in this repo is `1`; the upstream non-SD repo defaults to `0`.
- **`INTFLASH_BANK` must match how the bootloader was installed.** Dual-boot installs (`gnwmanager flash-patch ... --bootloader`) place retro-go in bank 2 (`INTFLASH_ADDRESS=0x08100000`); standalone installs use bank 1. The release build assumes bank 2.
- **Each emulator must fit in the per-core RAM budget — `RAM_EMU`, 724 KB**, not just compile. Memory regressions only show up at runtime on hardware. The budget is the *sum* of the core's overlay and its BSS, and the biggest cores are already at 94–99.8% of it. When a core's code+rodata will not fit, the escape hatch is to link it at a sentinel address and XIP it out of external flash — see `sm.xip` (`SM_CODE : ORIGIN = 0xDEAD0000` in the linker script, `store_file_in_flash_relocate()`), which is how Super Metroid keeps its cold code and rodata out of RAM.
- **`make help` is authoritative** for build flag names, defaults, and current values — prefer it over reading the Makefile.
- **Don't pass an over-aligned struct-member pointer straight into `memmove`/`memcpy`.** `arm-none-eabi-gcc 15.2` mis-marshalled the arguments of a `memmove` whose dest/src came directly from an `ABI_PTR_ALIGN` (`aligned(8)`) member inside a hot/cold–split (`.part.0`) function — it shifted the args one register over so a *pointer* landed in the size slot, producing a multi-hundred-MB copy that ran off into peripheral space and took an imprecise bus fault (EarthBound `scroll_window_up`). Materialize the pointer/size into plain locals (`uint16_t *dst = w->content_tilemap; ... memmove(dst, src, nbytes);`) and, if you suspect a codegen bug, **verify the arg registers in the disassembly**.

## Debugging crashes on hardware (BSOD / faults)

- **Faults do NOT self-label — the BSOD gives you a title, a PC and an LR, and nothing else.** `SCB->SHCSR` is never written (grep it: no hit anywhere in `Core/`), so BusFault/UsageFault/MemFault are all disabled and every one of them escalates to a plain **"Hardfault"**. `CFSR/HFSR/BFAR/MMFAR/ABFSR` are not read and not printed. So the title tells you nothing about *why*, and "Hardfault" is compatible with an alignment fault, a null deref, and a wild store alike. Budget for that: the PC is the whole clue. (Super Metroid's unaligned `STRD` came up as a bare "Hardfault"; had UsageFault been enabled the title alone — "Usagefault" — would have named it.)
- **If you do enable them**, `SCB->SHCSR |= USGFAULTENA|BUSFAULTENA|MEMFAULTENA` in `main()` makes the existing per-fault handlers in `stm32h7xx_it.c` fire and the title become "Busfault"/"Usagefault"/"Memfault". Then `CFSR` bit10 IMPRECISERR (`CFSR=0x…400`, BFAR invalid) = a buffered store to a no-slave address, whose reported PC is drain-time noise, not the culprit — and **`ABFSR` (`0xE000EFA8`)** names the bus interface the wild access used: bit2 **AHBP** = peripheral space `0x40000000–0x5FFFFFFF`, bit3 **AXIM** = all RAM/flash, bit0/1 = ITCM/DTCM.
- **GDB over an ST-Link (or pico-probe).** `gnwmanager gdbserver` spawns OpenOCD (auto-detects the probe via `interface/*.cfg`) with a gdbserver on `:3333`; then `make gdb` (or `arm-none-eabi-gdb build/gw_retro_go.elf -ex 'target extended-remote :3333'`). Use **`hbreak`, not `break`, for flash addresses** (`0x08xxxxxx`) — a software breakpoint silently fails to write flash. RAM/overlay addresses (`0x24xxxxxx`) take either. This gdb build has **no Python**; use native `-ex printf`/`x`. To catch a fault with full context, `hbreak common_fault_handler_c` and read `*frame` (the stacked r0–r3/lr/PC) plus live r4–r11.
- **Overlay RAM addresses alias.** Every core's overlay links at the same RAM_EMU VMA, so `gdb`/`addr2line` resolve a `0x24xxxxxx` address to *whichever* overlay's symbol it finds first (often zelda3/SMW, not the running core). Resolve EB addresses via `build/gw_retro_go.map` filtered to `build/earthbound/*.o`, or disassemble the specific `build/<core>/<file>.o`.
- **A hung boot ends at a rescue screen, not a flat battery** (`Core/Src/gw_boot_rescue.c`). The power button is a GPIO the firmware reads, so a hang used to be escapable only by draining the battery. Now: the watchdog is armed at the top of `main()` for every boot; a consecutive-failed-boot counter in **RTC backup register DR28** (don't reuse it — DR0 = OFW boot flag, DR1 = alarm epoch, DR29 = clock snapshot, DR30 = charger) stops the third failed boot at a rescue screen *before* SD/config/auto-resume, offering launcher-only boot / normal boot / power-off and powering itself off after 60 s; POWER on the BSOD really powers off. "Boot succeeded" = the shared input poll (`odroid_input_read_gamepad`) has run ≥300 times *and* ≥8 s of uptime; a deliberate sleep also clears the streak. The wiring spans five files — `tests/test_boot_rescue_wired.sh` pins every hook, `tests/test_boot_rescue.c` runs the real counter against a fake backup register.

## There are tests now, and a number

`tests/run.sh` is the suite; `tests/coverage.sh` measures it; `tests/coverage_scope.txt`
is the denominator, as data, with a reason per line. Coverage of the whole tree would be
a lie — it holds the ST HAL, 29 third-party cores, and drivers that cannot run on a host.
What is measured is the code we own and can run.

Three rules, each of which was learned the expensive way in one week:

- **A test must compile the file it claims to test.** `hw_jpeg_decoder.c` had three
  dedicated tests and **0% coverage**: all three reimplemented the HAL state machine
  instead of linking the driver. Three device-killing bugs shipped from that file while
  its tests were green. If your harness is a different program, it proves nothing — the
  same disease as `tools/sm_harness` above.
- **RED before GREEN, and RED against the real thing.** `tools/jpeg_harness/run.sh`
  compiles the actual pre-fix file out of git history (`git show 7ae5c0e8^:...`) and
  shows it failing. A test that has never failed proves nothing. The savestate round-trip
  written on day one never went red: it compared frame N+1 with N+2, and any cgram write
  during the run healed the stale cache before the hash saw it.
- **A safety net must not be the thing that breaks the build.** Twice in one day: the
  cross-overlay symbol check failed the build when `nm` was missing, and the JPEG runner
  failed it when CI's shallow clone had no pre-fix history to check against. Both teach
  people to ignore CI. Skip, say you skipped, and keep the real check strict.

## The bug is usually in the thing that never got wired

Not in the thing you are testing. Three of this week's failures were a caller that never
called:

- Super Metroid never called `common_emu_frame_loop()` — so no pacing, no frameskip, no
  speedup, no FPS counter. It never called `odroid_system_emu_init()` — so save and load
  did nothing at all.
- The clock app, added after the launcher's global "Idle power off" setting existed, has
  a loop of its own and never asked `odroid_idle_timeout_expired()`. It sat lit for ever
  at any setting.

No unit test of those functions could have caught any of it, because the functions were
fine. `tests/test_idle_timeout_wired.sh` is the shape of the test that can: it asserts
every loop that can idle asks the one rule, and that nobody re-derives it. Write that
kind of test when you add a contract, and again when you add a screen.

## Two traps that break everything quietly

- **`lang_t` is indexed by position** in the SD language binaries. Add a string only at
  the **end**; never delete or insert one mid-struct — every language file after it
  shifts. A retired string is commented `RETIRED ... slot kept, unused` and left in place.
- **A savestate is a raw dump of live structs.** Move a field and yesterday's file still
  opens, still reads to the end, and quietly restores nonsense — a black screen with the
  sound running, and no way to tell that from a bug. Stamp the file (magic/version/length)
  and refuse what this build did not write. And remember that a load restores *state*, not
  the caches derived from it: `ppu_saveload` has to invalidate the palette and brightness
  tables by hand, or the screen draws the scene you loaded in the colours of the one you left.

## Harness index

Every harness — the suite, the per-core `linux/` builds, the device-shaped
`tools/*_harness` rigs, the M4A prover, the on-device probe branch — is
catalogued in **[docs/HARNESSES.md](docs/HARNESSES.md)**: what each proves,
how to run it, and which one answers which question. Read it before writing a
new harness; the pattern you need probably exists.

## Emulator-specific notes

Detailed debugging guides live next to each porting layer (not in this file — keeps context lean when working on other cores). Cursor loads matching rules from `.cursor/rules/` when you edit files in those trees.

| System | Guide |
|--------|-------|
| PCE / PCE CD | [Core/Src/porting/pce/CLAUDE.md](Core/Src/porting/pce/CLAUDE.md) — harness `linux/Makefile.pce` |
| 32X | [Core/Src/porting/md32x/CLAUDE.md](Core/Src/porting/md32x/CLAUDE.md) — ⛔ performance axis CLOSED (measured); read before proposing anything |
| GBA | [Core/Src/porting/gba/CLAUDE.md](Core/Src/porting/gba/CLAUDE.md) — rig `tools/gba_m4a/` (M4A HLE proof + idle A/B + audio taps), idle-skip semantics, sound path, XIP contract |

Add a `CLAUDE.md` under `Core/Src/porting/<system>/` (and optionally `.cursor/rules/<system>.mdc`) when an emulator accumulates non-obvious debug knowledge.
