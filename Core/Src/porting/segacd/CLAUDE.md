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
`SEGACD_CODE_BASE` 0xDF000000). `lcd_setup_single_framebuffer()` also locks the
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
