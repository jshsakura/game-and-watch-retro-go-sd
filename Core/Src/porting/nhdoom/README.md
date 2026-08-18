# next-hack DOOM engine port (Phase 2 — Game & Watch HAL)

This directory is the Game & Watch / retro-go-sd hardware HAL for the
**next-hack nRF52840Doom** engine (`external/nh-doom`, GPL-2.0). It replaces the
doomgeneric checkpoint (`Core/Src/porting/doom`, kept in tree) for the DOOM
homebrew app, mirroring the Phase-1 host harness (`linux/nhdoom`) on real
hardware.

## Build gate

`USE_NHDOOM` (Makefile, default `1`) switches the DOOM target between engines:

* `USE_NHDOOM=1` — compile the 55-file next-hack engine + this HAL.
* `USE_NHDOOM=0` — restore the exact doomgeneric build (fully reversible; no
  nhdoom object is built and all nhdoom linker sections are empty).

## HAL files (mirror the host stubs in `linux/nhdoom/src`)

| device file        | replaces (engine seam)                  |
|--------------------|-----------------------------------------|
| `device_main.c`    | `src/main.c` + `doom_iwad.c` — entry, WAD XIP-cache, pointer-packing bases |
| `device_nrf.c`     | nRF NVMC/QSPI/GPIO backing + `qspi.h`   |
| `device_video.c`   | `src/graphics.c` + `src/display.c` — 240×240 8bpp → LUT8 LCD |
| `device_input.c`   | `src/keyboard.c` — G&W buttons → key bits |
| `device_audio.c`   | `src/pwm_audio.c` + `Doom/source/i_audio.c` (no-op) |
| `device_sys.c`     | nRF bits of `i_main.c` — `I_GetTime`/`I_Init`/`_putchar` |
| `include/`         | shadow headers (hardware-agnostic, copied from the host harness; `nrf.h` is device-specific) |

`i_main.c`, `i_audio.c`, `doom_iwad.c` are EXCLUDED from the engine source set
and replaced by the HAL, exactly as on the host.

## Flash / QSPI

The engine reads WAD/patch data from external flash. On the G&W that flash is
memory-mapped OCTOSPI XIP (`0x90000000`), so no DMA is needed:

* `i_spi_support.h` reads are plain pointer derefs of the XIP base.
* `r_fast_stuff.c`'s `NRF_QSPI` DMA fast-path is satisfied by a synchronous
  shim: `#define NRF_QSPI nh_qspi_service()` (see `include/nrf.h`) flushes any
  pending `memcpy(DST,SRC,CNT)` and raises `EVENTS_READY` on every register
  access — single-core analog of the host's polling thread, zero engine edits.

## 256 KB short-pointer window

The engine packs zone-block pointers as 16-bit `(offset>>2)` relative to a
256 KB-aligned `RAM_PTR_BASE`. Only `staticZone[113600]` (`.displayData_bss`) is
short-pointer'd, so it must sit in one 256 KB window. The linker
(`STM32H7B0VBTx_SDCARD.ld`, `.overlay_doom_bss`) `ALIGN(0x40000)`s, publishes
`nh_ram_window_base`, and places staticZone + the engine's other writable bss
there, in **AXI SRAM (RAM_EMU)** — DTCM is only 128 KB < the ~193 KB footprint.
`app_main_nhdoom` sets `RAM_PTR_BASE = nh_ram_window_base` at runtime (mirrors
host `host_main.c`), so the `|RAM_PTR_BASE` packing is exact.

## WAD pipeline (NOT bundled)

The engine needs the **MCUDoomWadUtil-converted** IWAD, not a raw `DOOM1.WAD`.
Produce it with the converter shipped in the engine repo
(`external/nh-doom/MCUDoomWadUtil`, a Code::Blocks C project):

```
# build MCUDoomWadUtil (Code::Blocks / gcc), then, with gbadoom.wad in the cwd:
MCUDoomWadUtil  DOOM1.WAD  doom1.mcu.wad
```

* The converter REQUIRES `gbadoom.wad` (the GBA-Doom reference WAD) in its
  working directory; it emits an IWAD-magic (`'I'`) `.mcu.wad`.
* Copy the result to the SD card at the path the firmware expects:

```
/roms/homebrew/doom1.mcu.wad      (NHDOOM_WAD_PATH in device_main.c)
```

`app_main_nhdoom` caches that file into XIP flash via
`odroid_overlay_cache_file_in_flash()` (CRC-cached across launches), then sets
`doom_iwad`/`p_doom_iwad_len` to the mapped pointer — never a static
initializer (the base is runtime, exactly like the host).
