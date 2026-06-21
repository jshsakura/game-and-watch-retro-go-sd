# DOOM (and Wolfenstein 3D) Homebrew Port — Plan

Status: **planning**. Goal: ship DOOM as a self-contained homebrew overlay app, exactly
mirroring the Music player integration pattern, then reuse the same scaffold for Wolf3D.

## Why this is feasible (vs GBA, which is not)

- The Game & Watch (STM32H7B0, Cortex-M7 @ ~280 MHz) has **~724 KB** of `RAM_EMU`
  overlay budget, plus 128 KB DTCM / 64 KB ITCM / 128 KB AHB SRAM.
- The **identical MCU** already runs doomgeneric: `ghidraninja/game-and-watch-doom`
  (same `STM32H7B0VBTx_FLASH.ld`, same 320-wide LCD). We re-host a proven port inside
  the retro-go overlay framework rather than porting from scratch.
- GBA was ruled out: no Cortex-M7 (Thumb-2) dynarec exists; even SNES is not emulated
  here (Zelda3/SMW are native C re-implementations); WonderSwan barely fit the RAM budget.

## Base selection

- **Engine base:** `ozkl/doomgeneric` (GPL-2.0) — clean 6-function `DG_` platform API.
- **Glue reference:** `ghidraninja/game-and-watch-doom` — already solves LCD blit,
  buttons, and WAD-from-flash on this exact MCU. Mine its platform layer.
- **RAM-tuning references (fallback only):** `cnlohr/embeddedDOOM` (heap as low as
  ~120 KB for E1M1, ~288 KB recommended), `kilograham/rp2040-doom` (zone heap ~45 KB by
  reading compressed WAD straight from flash).

## RAM budget

| Region | Size | DOOM use |
|---|---|---|
| `.overlay_doom` (RAM_EMU) | ~724 KB | code + .bss (~171 KB) + zone heap (280–512 KB) |
| DTCM | 128 KB | stack, scanline buffers (no-cache fast path) |
| ITCM | 64 KB | renderer hot loops (column/span drawers) |
| AHB SRAM | 128 KB | DMA framebuffer / scratch |
| SPI flash (XIP) | 64 MB | **WAD read-only, no RAM cost** |

Verdict: 724 KB comfortably holds a generous zone heap for all of shareware Episode 1.
No exotic size tuning required for first boot.

## WAD strategy

- Ship the freely-redistributable shareware `DOOM1.WAD` (or a minimized derivative via
  `wadptr` like the G&W port's <0.25 MB build). **Do NOT bundle commercial WADs** (GPL
  covers the engine; the retail IWADs are not redistributable).
- Store the WAD in SPI flash and access it **read-only via the XIP memory-mapped
  address** — intercept doomgeneric's `W_*`/`I_*` file reads to point at the flash
  address instead of `fopen`/`fread`. No RAM copy.

## Integration steps (mirror the Music app)

The Music app builds into `.overlay_music`, exposes
`app_main_music(load_state, start_paused, save_slot)`, and ships as
`sd_content/roms/homebrew/Music.bin`. DOOM mirrors this:

1. **Submodule**: add `external/doomgeneric` (ozkl).
2. **Linker**: add `.overlay_doom` / `.overlay_doom_bss` at `__RAM_EMU_START__` in
   `STM32H7B0VBTx_FLASH.ld` — clone the `.overlay_tgb`/`.overlay_music` block, including
   the `ASSERT(... < __RAM_EMU_END__)` overflow guard. **(risky edit — do carefully)**
3. **Glue**: `Core/Src/porting/doom/` — implement the `DG_` API + WAD-from-XIP + zone
   init pinned to the overlay region. Entry point `void app_main_doom(...)`.
4. **Build**: compile doomgeneric + glue into `build/doom/*.o`, link into the overlay,
   objcopy `--only-section=.overlay_doom` → `sd_content/roms/homebrew/Doom.bin`.
5. **Launcher**: register/dispatch by name like Music (`strcmp(name, "Doom")`), under the
   existing Homebrew tab (now with the beer-stein icon).

### Glue surface (the whole port)

| `DG_` / hook | Maps to |
|---|---|
| `DG_Init` | LCD framebuffer + input init; set zone heap base into `.overlay_doom` |
| `DG_DrawFrame` | blit 320×200 → 320×240 framebuffer via `gw_lcd` (DMA; letterbox) |
| `DG_GetKey` | `gw_buttons` → DOOM keycodes (needs a modifier scheme; only ~6 buttons) |
| `DG_GetTicksMs` / `DG_SleepMs` | systick/RTC; frame pacing |
| `DG_SetWindowTitle` | no-op (or top bar) |
| WAD `W_*`/`I_*` | read from XIP flash address (no RAM copy) |
| malloc / `Z_Init` | heap base+size pinned to overlay region |
| sound | **stubbed initially** (G&W DOOM shipped sound-stripped); wire `gw_audio` later |

## Risks

1. **Performance, not RAM** (most likely blocker): sustaining playable FPS while
   blitting/scaling and reading the WAD over XIP SPI on a cache-sensitive M7. Mitigate:
   renderer hot path in ITCM, framebuffer in AHB/DTCM with DMA, WAD in *cached* XIP,
   overclock. (This repo's own video-player notes show flash/SD bandwidth is the
   recurring constraint on this class of device.)
2. **Overlay BSS overflow** — guard with the same `ASSERT(... < __RAM_EMU_END__)`.
3. **WAD-from-XIP wiring** — doomgeneric assumes stdio; intercept `W_*`. (ghidraninja
   port already solves this — port it.)
4. **Input ergonomics** — ~6 buttons vs DOOM's control set; design a modifier mapping.

## Wolfenstein 3D (after DOOM)

Lighter than DOOM (raycaster, flat 90° walls, no BSP/variable heights → smaller RAM/CPU).
Reuse the same overlay scaffold and glue (framebuffer/input/timing); swap the engine for
a `wolf4sdl`-derived bare-metal core. Treat as a near-free follow-on once DOOM's platform
layer exists.

## Licensing

doomgeneric and the G&W DOOM port are **GPL-2.0** (DOOM source lineage). retro-go is
already GPL, so this is compatible: keep GPL headers, ship the glue source under GPL-2.0,
ship only the shareware/redistributable WAD.

## Sources

- doomgeneric — https://github.com/ozkl/doomgeneric
- game-and-watch-doom (same MCU) — https://github.com/ghidraninja/game-and-watch-doom
- embeddedDOOM (low-RAM numbers) — https://github.com/cnlohr/embeddedDOOM
- rp2040-doom (zone/WAD-in-flash) — https://kilograham.github.io/rp2040-doom/speed_and_ram.html
