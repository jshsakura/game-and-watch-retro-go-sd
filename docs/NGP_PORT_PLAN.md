# Neo Geo Pocket (+ WonderSwan) port plan

Status: **scaffolding** — core analysed, build wiring pending, ROM-memory
strategy is the open design decision.

Worktree: `gw-ngp-pocket`, branch `feat/ngp-pocket`, based on clean
`origin/main` (935a2c19). The `music-player` working tree is untouched.

## Core choice

- **NGP/NGPC → `libretro/RACE`** submodule at `external/race`.
  Performance-focused (made for slow handhelds), portable C.
  - Use the **CZ80** C Z80 core (`-DCZ80`). EXCLUDE `DrZ80_support.c`
    (ARM32 assembly — invalid on Cortex-M7 / Thumb-2).
  - EXCLUDE `external/race/libretro/**` (the libretro wrapper). We provide
    our own front-end glue.
- **WonderSwan → `oswan` (Cygne lineage).** Already vendored as the
  `external/oswan-go` submodule (sylverb fork; not yet `--init`'d, no Makefile
  target). Pure C, pure-C V30MZ (`emu/cpu/nec.c`), **no C++ runtime**. ROM is
  already a 256-entry pointer page table `ROMMap[256]` → XIP-from-flash is a
  few-line change (graft `xrip/pico-wonderswan`'s zero-copy flash-pointer
  model). Saves in separate buffers (`RAMMap[]`/`IEep[]`), never in ROM.
  API: `WsInit/WsCreate/WsReset/WsRun()` per frame; `FrameBuffer` 224x144
  RGB565; input via global `ButtonState`; `WsSaveState/WsLoadState`.
  `APPID_WSWAN` already exists. Beetle Cygne is the only other pure-C option
  but copies the whole (≤16MB) ROM to RAM — disqualified for this RAM budget.
  Glue mirrors PCE/WSV exactly; RTC `time()/localtime()` (`WS.c:500`) to stub.

## RACE API (front-end contract)

- `void mainemuinit(void)` — init after `system_sound_chipreset(rate)`.
- `int handleInputFile(name, romData, romSize)` — load ROM (see ROM memory).
- `void tlcs_execute(CPU_FREQ/60, skipFrame)` — run one frame. `CPU_FREQ = 6144000`.
- `void graphics_paint(unsigned char render)` — **front-end MUST define this**
  (it lives in `libretro.c`, which we don't compile). The renderer writes
  pixels straight into `screen->pixels`, so our `graphics_paint` is a no-op /
  present-flag.
- `extern struct ngp_screen* screen;` — `{ int w, h; void *pixels; }`. Set
  `w=160 (FB_WIDTH)`, `h=152 (FB_HEIGHT)`, `pixels = framebuffer` (RGB565).
  Render stride is `screen->w`.
- `extern uint8_t ngpInputState;` — write button bits each frame:
  `A=0x20 B=0x10 RIGHT=0x08 LEFT=0x04 UP=0x01 DOWN=0x02 OPTION/START=0x40`.
- Audio: `sound_update(buf, n*2)` then `dac_update(buf, n*2)` (mono int16,
  `n = rate/60`); OR the band-limited `neopop_blip_flush` path. Start with the
  simple `sound_update`+`dac_update` path.
- Save state: `int state_get_size(void)` (= `sizeof(race_state_t)`),
  `state_store_mem(void*)`, `state_restore_mem(void*)`.
- Pixel convert: `NGPC_TO_RGB565(col)` (palette LUT). Screen 160x152.

## ROM memory — THE design decision

`mainrom[MAINROM_SIZE_MAX]` (`MAINROM_SIZE_MAX = 4 MiB`) is a static RAM
array. `handleInputFile` `memcpy`s the whole ROM into it. **4 MiB cannot fit
in `RAM_EMU` (~700 KiB).** And `flashWriteByte` (flash.c) writes cart saves
*into* `mainrom`, so it cannot be pure read-only flash-XIP either.

Options:

- **(A) Cap + RAM copy** — shrink `MAINROM_SIZE_MAX`, keep RAM copy. Only
  ~≤512 KiB ROMs fit. Quick, but excludes most of the interesting library
  (KoF, fighters are 2–4 MiB). Saves work in RAM, just persist on exit.
- **(B) Flash-XIP read + copy-on-write save blocks** *(recommended)* — keep
  the ROM memory-mapped read-only in external flash
  (`odroid_overlay_cache_file_in_flash` returns an XIP pointer); make
  `mainrom` a pointer to it. Redirect the (small) writable cart-flash blocks
  through a RAM copy-on-write buffer so `flashWriteByte` targets RAM, not
  flash. Full ROM compatibility; RAM = `mainram`(224K) + screen(47K) + small
  COW buffer. More core patching.

`mainram[(64+32+128)*1024]` = 224 KiB fits fine regardless.

## Integration template = WSV (potator)

Mirror these exactly for NGP (`build/ngp`, `.overlay_ngp`, `app_main_ngp`):

- `Core/Src/porting/ngp/main_ngp.c` + `Core/Inc/porting/ngp/main_ngp.h` — glue.
- `Makefile` / `Makefile.common` — `CORE_NGP`, `NGP_C_SOURCES`, overlay build/link.
- `STM32H7B0VBTx_FLASH.ld` + `_SDCARD.ld` — `.overlay_ngp` + `.overlay_ngp_bss`
  at `__RAM_EMU_START__` (overlays share RAM; only one app runs at a time).
- `Core/Inc/gw_linker.h` — `_OVERLAY_NGP_{LOAD_START,SIZE,BSS_START,BSS_END,BSS_SIZE}`.
- `Core/Src/retro-go/rg_emulators.c` — `#include "main_ngp.h"`, an
  `emu_dispatch_t emu_ngp { "/cores/ngp.bin", ... EMU_ENTRY(app_main_ngp) }`,
  a `run_internal_emu` branch on system name, and
  `add_emulator("Neo Geo Pocket", "ngp", "ngp ngc ngpc", ...)`.
- New `APPID_NGP` in `appid.h`.
- ROMs scanned from `/roms/ngp/` (`.ngp/.ngc/.ngpc`), like other cores.

Display: 160x152 → 320x240. Reuse WSV nearest-neighbour + bilinear blits
(they read `video_frame.w/h`, source-agnostic). Skip WSV's `v3to5`/`jth`
scalers (tuned for 160x160; 152 rows overflow).

## Logos (deferred)

Wire `RG_LOGO_*_NGP` slots with a text placeholder. Console/brand logo art is
NOT produced here (IP). If used, the user drops their own
`icons/c_ngp.bmp` (54x32) / `icons/h_ngp.bmp` (138x18), 24bpp BMP3.
