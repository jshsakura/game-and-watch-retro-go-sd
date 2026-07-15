# Experimental: SNES (Super Famicom) core

A new **SNES / Super Famicom** launcher tab, running a generic LakeSnes core built
from the same emulation sources as the Super Metroid port but interpreting the
game's 65816 code. This is an **experimental first cut** — flash it to try SNES on
hardware and report how it runs.

## What's in it

- **SNES tab** — put `.sfc`/`.smc` ROMs in `/roms/snes/` on the SD card. LoROM and
  HiROM mappers boot; **enhancement-chip carts** (SA-1, SuperFX, DSP, Cx4, S-DD1…)
  are rejected at load with a message instead of hard-faulting mid-game.
- **Savestates** — save/load is wired and stamped; a two-process cold-resume test
  passes on A Link to the Past, Donkey Kong Country and Turtles in Time (found and
  fixed two real defects along the way: controller shift-register serialization and
  truncated-file acceptance).
- **Icon + header** from the existing platform icon set; the tab reads
  **"SNES (SUPER FAMICOM)"**.

## Requirements — copy **all three** to the SD card

1. Flash `retro-go_update.bin` (normal update flow — do **not** full-flash).
2. Copy `cores/snes.bin` → SD `/cores/`.
3. Copy `bios/logo.bin` → SD `/bios/` (without it the SNES header wordmark won't
   show; the colour icon still does).

Then put a ROM in `/roms/snes/`.

## Please read before flashing

- **Settings reset once.** This build adds a system, which grows the saved config
  struct; the launcher discards the old file, so **language, volume, backlight and
  coverflow return to defaults** on first boot. This is deliberate and one-time.
- **Not yet verified on hardware.** All correctness is proven in host/QEMU
  simulation — the device is the final judge. Expect rough edges; performance,
  audio pitch and save round-trips on real hardware are exactly what we need
  feedback on.
- **Shared emulation code changed.** The SNES work also touches the rendering code
  the Super Metroid port shares (a sprite-line cache and paired blit, proven
  output-identical in simulation). If Super Metroid looks different, that's the
  thing to report.

## Known limitations

- Speed optimizations proven in simulation (a 65816→C translator, an idle-skip, and
  native N-SPC audio) are **not** in this baseline build — they land in later
  releases once calibrated against real-hardware timing.
- Sound uses the full SPC700 emulation (accurate, not yet the faster native path).
- Titles needing enhancement chips do not run (DSP-1 support is a work in progress
  and inert here).

_This is a testbed pre-release; not for daily use._
