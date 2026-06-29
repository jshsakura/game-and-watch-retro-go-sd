# Game.com host harness

Headless PC harness for the ported Tiger Game.com core (Sharp SM8500/SM8521).
Same approach as the ZX / Lynx / PCE host harnesses: deterministic, no SDL,
dumps a PPM and a boot-signal trace so we can confirm boot+play on PC **before**
touching the device.

## Files (portable C, reusable by a future device port)
- `sm8500.c` / `sm8500.h` — CPU core (port of MAME `cpu/sm8500`)
- `sm85ops.h` — opcode table, **verbatim from MAME** (BSD-3-Clause, Wilbert Pol)
- `gamecom_core.c` / `gamecom_core.h` — machine: bus, banking, blitter-DMA,
  timers, scanline renderer (port of MAME `gamecom_m.cpp` / `gamecom_v.cpp`)
- `main.c` — headless driver (load ROMs → run N frames → dump PPM + trace)

## Build & run
```sh
cd linux
make -f Makefile.gamecom
./build/gamecom <internal.bin> <kernel.bin|-> <cart.bin|-> [frames] [tap_x tap_y tap_start tap_len]
```
The optional `tap_*` args script one touchscreen tap (screen pixels, 200×160).
The PDA menu is touch-only; e.g. tapping the CARTRIDGE icon (~45,60) launches the
inserted cart:
```sh
./build/gamecom internal.bin external.bin frogger.bin 700 45 60 400 120
```
Verified chain: BIOS → Tiger logo → PDA menu → tap CARTRIDGE → Frogger copyright →
Frogger gameplay (river/road/frog/score). Touch grid = 13 cols × 10 rows,
col=x/16, mask=1<<(y/16) (from MAME `layout/gamecom.lay`).

## ROMs you must supply (copyrighted Tiger images — NOT in this repo)
The harness needs the console firmware, exactly like the ZX harness needs
`48.rom`. Drop these next to the binary (or pass paths):

| File          | Size   | MAME CRC32 | Maps at        |
|---------------|--------|-----------|----------------|
| `internal.bin`| 4 KB   | a0cec361  | 0x1000–0x1FFF  (reset vector region; PC starts 0x1020) |
| `external.bin`| 256 KB | e235a589  | "kernel" PDA firmware (default bank source) |
| a cartridge   | 32KB–2MB | —       | `.bin`/`.tgc`, banked via MMU1–4 |

Supported cart sizes: 0x8000, 0x40000, 0x80000, 0x100000, 0x1c0000, 0x200000
(smaller images are mirrored up, matching MAME's loader).

## What the trace tells you
- `first non-blank frame` ≥ 0  → the LCD turned on and something rendered (boot progressed)
- `peak non-blank pixels`      → how much got drawn
- `PC` samples                 → where the CPU is (detect halts/loops)

`gamecom_frame.ppm` is the final 200×160 framebuffer (5-grey palette).

## Status
✅ CPU, bus, banking, blitter-DMA, scanline renderer and **touchscreen input** all
verified — Frogger runs to playable gameplay on PC. Audio (SG0/SG1/DAC) is parsed
but not yet synthesised. The real-time clock (CLKT/CK_INT) is a coarse frame-based
approximation. Next: device port (note the G&W has no touchscreen, so the PDA menu /
touch games will need a virtual-cursor stylus scheme — d-pad-moved crosshair + tap).
