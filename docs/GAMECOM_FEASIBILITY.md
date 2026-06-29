# Tiger game.com — Feasibility & Port Plan

> Status: **feasibility study only, no code written** (2026-06-30).
> Verdict: **technically feasible, memory is NOT the constraint — the work is a from-scratch CPU-core port.** Low priority vs. the already-planned O2EM/ZX/C64 line (see `NEW_SYSTEMS_PLAN.md`), all of which have ready-made portable cores.

---

## 1. What game.com is

Tiger Electronics handheld (1997), ~20 commercial titles (mostly poorly reviewed). First handheld with internal save memory and a touchscreen. Relevance to us: it is one of the **lightest possible systems to emulate** in terms of RAM, but it has **no portable emulator core in existence**.

## 2. Hardware facts (researched)

| Item | Spec | Port impact |
|---|---|---|
| CPU | **Sharp SM8521** (SM8500 family), 8-bit, real clock ≈ 4.9152 MHz | ⚠️ **The whole problem.** Custom ISA, only emulated inside MAME. |
| Internal RAM | 1 KB (`$0000–$03FF`) | trivial |
| Extended RAM | 8 KB (`$E000–$FFFF`) | trivial |
| VRAM | 16 KB (`$A000–$DFFF`, write-only) | trivial |
| Internal ROM (BIOS) | 4 KB (`$1000–$1FFF`) | **BIOS dependency** — must be bundled/cached like ZX `48.rom` |
| Cartridge | 8 KB × 4 banks (`$2000–$9FFF`), MMU1–4 banking | maps cleanly onto **flash-XIP** ROM model |
| Display | 200×160, 4-level grayscale | fits G&W 320×240 LCD directly (center or scale) |
| Sound | 8-bit mono | simple |

**Emulated RAM footprint ≈ 30 KB total.** This is far below anything that has caused RAM trouble on this project (DOOM, PCE-CD, Lynx). The `[[ram-budget-principle]]` is *favorable* here — game.com would be among the cheapest cores in the tree.

### MAME address map (reference)
```
0x0000-0x03FF  RAM (shared with maincpu)
0x0014-0x0017  PIO (buttons)
0x0020-0x007F  CPU internal registers
0x1000-0x1FFF  Internal ROM (4KB BIOS)
0x2000-0x3FFF  Bank 1 (cart, MMU1)
0x4000-0x5FFF  Bank 2 (cart, MMU2)
0x6000-0x7FFF  Bank 3 (cart, MMU3)
0x8000-0x9FFF  Bank 4 (cart, MMU4)
0xA000-0xDFFF  VRAM (16KB, write-only)
0xE000-0xFFFF  Extended I/O & RAM (8KB)
```

## 3. The core-availability problem (the real blocker)

Unlike every system added so far, **game.com has no reusable portable core**:

- No libretro core. No standalone C emulator. RetroArch itself runs game.com **through the MAME core**.
- The only complete implementation is **MAME**: driver `src/mame/tiger/gamecom.cpp` (~332 lines) + the **SM8500 CPU core** (`cpu/sm8500/`), both C++ and bound to the MAME device framework.
- MAME's own emulation has **known accuracy gaps** — wrong clock and wrong instruction cycle counts ([mamedev/mame#7303](https://github.com/mamedev/mame/issues/7303)).

Contrast with the existing tree: every other core (tgbdual, fceumm, smsplusgx, gwenesis, handy, oswan, race, o2em, Frodo, chips/ZX…) was a **forked portable core wired in**. game.com would be the first that requires **porting a CPU core out of MAME C++ into this project's freestanding C porting layer**.

## 4. Integration pattern in THIS repo (what a new system costs)

Confirmed by codebase scan. The framework handles input/audio/save plumbing; you supply a porting layer + register it.

**Files to create**
- `Core/Src/porting/gamecom/main_gamecom.c` — entry `app_main_gamecom(load_state, start_paused, save_slot)`; calls `odroid_system_init` / `odroid_system_emu_init(LoadState, SaveState, …)`; per-frame: input → run frame → blit 200×160 → `common_emu_frame_loop()`. (Template: `Core/Src/porting/zx/main_zx.c`, ~158 lines.)
- `Core/Inc/porting/gamecom/main_gamecom.h` — single declaration.
- `Core/Src/porting/gamecom/sm8500/…` + driver glue — **the ported MAME SM8500 CPU core + gamecom machine logic** (this is the bulk of the work).
- `linux/Makefile.gamecom` + `linux/gamecom/` — PC/SDL2 host harness (template: `linux/Makefile.nes`).

**Files to edit**
- `Core/Src/retro-go/rg_emulators.c` — `#include "main_gamecom.h"`; bump `MAX_EMULATORS` 26→27 (line ~417); `add_emulator("game.com","gamecom","bin", RG_LOGO_PAD_*, RG_LOGO_HEADER_*, …)`; dispatch in `emulator_run()` (`run_internal_emu(&emu_gamecom, …)` — it's small enough to link internally, no overlay `.bin` needed).
- `Core/Inc/retro-go/bitmaps.h` — append `RG_LOGO_PAD_GAMECOM` / `RG_LOGO_HEADER_GAMECOM` (append at end to avoid shifting indices) + logo art.
- `Makefile` / `Makefile.common` — `GAMECOM_C_SOURCES`, `GAMECOM_OBJECTS`, includes, pattern rule, `mkdir $(BUILD_DIR)/gamecom`, link line, vpath.

**ROM/BIOS/save**
- ROM: flash-XIP via `odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false)` — carts are tiny, fits easily; 8 KB bank windows map onto MMU1–4.
- BIOS: cache `/bios/gamecom/internal.rom` (4 KB) in flash, same pattern as ZX `48.rom`.
- Save/load: implement `SaveState`/`LoadState` serializing CPU+RAM+VRAM+bank regs to `/savestates/gamecom/<rom>.slotN`. State is small (~30 KB) → trivial.

## 5. Effort estimate

| Task | Size | Risk |
|---|---|---|
| Port SM8500 CPU core (MAME C++ → freestanding C) | ~1.5–2.5K lines | **High** — custom ISA, cycle accuracy, MAME device-framework decoupling |
| Port gamecom driver/machine (banking, VRAM→LCD, PIO/input, timers) | ~330 lines + glue | Medium |
| Sound (8-bit mono) | small | Low |
| `linux/` host harness (prove boot+play on PC first) | small | Low |
| Device wiring (registration, Makefiles, logo, save) | mechanical | Low |

**Net:** memory/flash budget is a non-issue; the cost is concentrated entirely in **porting a CPU core that today exists only inside MAME**, plus reconciling MAME's known timing inaccuracies. Payoff is small (tiny, weak library).

## 6. Recommendation

- **Feasible, but lowest ROI** of the candidate systems. Finish the planned `[[three-systems-initiative]]` (O2EM, ZX Spectrum, C64 — all have portable cores already in-tree or trivially wireable) first.
- **If pursued, gate it on a host-harness PoC first** (`[[host-harness-initiative]]`, fact/log-based per `[[log-based-analysis]]`): port SM8500 + gamecom into `linux/`, get a real game to boot and run on PC with screenshots, *before* any device flash. Same approach that de-risked PCE-CD and Lynx.
- Licensing note: lifting code from MAME means **GPL/BSD-MAME license obligations** — must be respected in this repo's submodule/attribution structure before shipping.

## 7. References
- MAME driver: `src/mame/tiger/gamecom.cpp`; CPU: `src/devices/cpu/sm8500/`
- Clock/cycle accuracy issue: https://github.com/mamedev/mame/issues/7303
- Hardware overview: Video Games Museum, Racketboy game.com 101, RetroReversing game.com
</content>
</invoke>
