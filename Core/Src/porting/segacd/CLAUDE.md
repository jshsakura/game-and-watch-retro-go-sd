# Sega CD / Mega CD port (PicoDrive)

The core is PicoDrive's MCD code from `external/picodrive` (the same submodule
the 32X core uses), built with `GNW_MCD_SPLIT` + `GNW_MCD_BIOS_XIP`. The earlier
gwenesis-based engine and its `pd_cd/` fork are gone; anything in git history
under `segacd_engine.c` / `segacd_bus.c` describes a different program.

## Where the memory is

The whole reason this core fits: PicoDrive asks for its big memories through
`plat_mmap()` with tagged addresses, and `main_segacd.c` answers each tag from a
different SRAM domain.

| Tag | What | Lives in |
| --- | --- | --- |
| `0x05000000` | `mcd_state` | AHB (`ahb_calloc`) |
| `0x05100000`, PRG pages 0-1 | 3 x 64K | LCD bonus pool (LUT8, **one** framebuffer) |
| PRG page 2 | 64K | `.segacd_ahb` at `0x30000000` |
| PRG page 3 | 64K | AHB heap |
| PRG pages 4-6 | 3 x 64K | AXI (`segacd_prg_axi`, overlay BSS) |
| PRG page 7 | 64K | DTCM heap (`calloc`) |
| `0x05300000` | Word RAM 256K | AXI (overlay BSS) |
| `0x05400000` / `0x05408000` | PCM RAM | SRAM4 (32K, real size) / AHB |
| `0x05500000` | BRAM 8K | DTCM heap |

BIOS and cold code/rodata are XIP'd from external flash (`segacd.xip`, sentinel
`SEGACD_CODE_BASE` 0xDF000000). The hot CPU and tile code rides ITCM
(`.overlay_segacd_itc`, 57 KB of 64): without it the intro of Final Fight CD
drew 14 fps at 100% busy, with it 60 at ~62%. The 68K handler list is
generated (`tools/segacd_itc_gen.py` -> `segacd_itc_ops.ld`); append to that
section, do not reorder it, and re-measure on the device if you change it.
`cache_xip()` patches sentinels in the RAM overlay and in ITCM, and
`scripts/check_xip_sentinels.py` fails the link if an instruction would be
mistaken for a sentinel. `lcd_setup_single_framebuffer()` also locks the
LCD to one buffer until reboot, so a fault redraw cannot zero the PRG pages
behind it. The DTCM heap is the tight one: do not add a heap allocation on the
load path (see `segacd_bram_load`, which reads straight into BRAM for that
reason).

## Behaviour a player sees, and where it lives

- **BIOS by disc region** (`bios_path_for_region`): `PicoCdCheck` reads the
  region from sector 0. Japanese discs (every Korean release tested) need
  `bios_CD_J.bin`.
- **START through the BIOS menu** (`Core/Inc/porting/segacd/segacd_boot_start.h`):
  the JP/EU BIOS waits at its menu with a disc in. Pulsed until the main 68K
  first runs outside the BIOS ROM window, never after a state load.
- **Never patch a disc's sector 0.** A JP disc's IP code starts right after its
  (short) security block at 0x356; pasting the US block over 0x200-0x783 erased
  it and made nine games hang at the BIOS exception stub `0x210`. The history is
  in docs/OPTIMIZATION_LEDGER.md.
- **Controls** come from the MD keymap profile (`odroid_settings.c`
  keymap_profiles; `APPID_SEGACD` shares `APPID_MD`'s), so the in-game Controls
  dialog remaps them. 6-button pad (`PICO_INPUT_PAD_6BTN`). The pad is computed
  after the menu/turbo handlers, with the Mario TIME/PAUSE swap undone.
- **In-game saves** are BRAM, persisted to `<rom>.brm` (the SRAM path): loaded
  after `PicoLoadMedia` (which formats it), written when its CRC changes (checked
  every 60 frames) and from the `sram_save` hook. **A savestate load does not
  roll BRAM back**: `segacd_load` flushes it first and reloads the file after.
- **Savestates** are `SCDP` magic + version + `PicoStateFP`. A round trip on the
  host matches an uninterrupted run from the second frame on; the first frame
  after a load can differ in its top lines.
- **Borders**: a mode change (H40/H32, 224/240 lines) wipes the frame to
  `SEGACD_BORDER_INDEX` (0xE0, pinned black by PicoDrive), and so does a load;
  otherwise the old mode's pixels stay in the margins as colored bars.

## Host rig

`tools/pico_host_segacd/run.sh <game.cue> [frames] [pattern]` compiles the
Makefile's picodrive sources with the device's defines and reads the device's
own `segacd_config.h` (options, border index) and `segacd_boot_start.h`.
Pattern `auto` is what the device does. `STATE_OUT`/`SAVE_AT` +
`STATE_IN`/`FRAME0` give a frame-aligned save/load round trip; `BRAM_IN`/
`BRAM_OUT` exercise BRAM persistence. It is not a speed instrument.

## Palette and menus (full-palette LUT8)

PicoDrive's 8-bit renderer uses CLUT slots 0x00..0xBF, so the shared LUT8
scheme (32 cart colours, darkened twins at +0x20, menu colours at 0x40) does
not fit: menus drew in game colours, the dim OR'ed into game slots, and
savestate thumbnails were saved with an all-zero 32-entry palette (black).
`segacd_lcd_clut.c` registers `lcd_clut_full_ops_t`: menu colours at 0xF8..,
a black at 0xFD, the dim darkens the palette (the next frame's push restores
it), and thumbnails are written as RGB565. `tests/test_lcd_clut.c` models the
LTDC CLUT and checks what the panel shows.

## Clock

`common_emu_auto_oc(2)` (340 MHz). Stock 280 MHz held Final Fight CD's intro at
60 fps, but CD audio streaming reads the SD card over polled SPI (~33% of the
CPU on Orgun's title) and at 280 MHz that dropped frames (the status bar read
45-50). The call is a floor: a higher launcher setting still wins.

## One framebuffer: scaling, tearing, menus

The core keeps one LUT8 framebuffer (the LCD pool's other half holds PRG RAM),
so the panel scans out the buffer being drawn. PicoDrive renders each line
into its own line buffer (`PicoDrawSetOutBuf(NULL, 0)`) and `segacd_scan_end()`
places it: stretched by the launcher's Scaling option (FIT: H32 to 320 via
POPT_EN_SOFTSCALE; FULL/CUSTOM also 224 to 240 lines), and only after the beam
(LTDC CPSR) has passed that row, so a refresh never shows two frames. Starting
at vblank instead left a fixed seam mid-screen: the SD reads at frame start let
the beam overtake the renderer, which caught up halfway down.
Nothing redraws outside the picture, so `segacd_clear_borders()` clears it each
drawn frame. Menus: `lcd_clear_active_buffer()` is a no-op under the single-FB
lock (it wiped the only image), and `segacd_repaint()` redraws the frame with
`PicoFrameDrawOnly()` once per menu session (every pass flickered the dialog).

