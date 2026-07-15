---
id: systems
title: Supported systems
---

# Supported systems

🧪 = added or enabled by this Lab fork · everything else is upstream and documented in the
[upstream README](https://github.com/sylverb/game-and-watch-retro-go-sd).

| System | Emulator / port | Origin | Notes |
| --- | --- | --- | --- |
| **Game Boy Advance** | gpSP | 🧪 Lab | Pokémon Ruby & Emerald **full speed**; heavier titles vary. ROM stays in flash (never in RAM). [Details](./game-boy-advance.md) |
| **PC Engine CD** | pce-go + CD | 🧪 Lab | CD-DA, ADPCM, BRAM saves, savestate/resume. `/roms/pcecd/<game>/`. 4 playthroughs verified |
| **Atari Lynx** | handy | 🧪 Lab | in-game save/load + resume; 512K carts run from flash when RAM is tight |
| **WonderSwan / Color** | oswan | 🧪 Lab | 8 MB carts (One Piece), sound-DMA boot hang fixed, FIT-scaling fixed |
| **Neo Geo Pocket / Color** | RACE | 🧪 Lab | runs from flash, flicker/scaling fixes, sound resumes after loading a state |
| **Virtual Boy** | red-viper | 🧪 Lab | ⚠️ **~65-70% speed** with auto-overclock; gapless audio, selectable pad presets |
| **Commodore 64** | Frodo | 🧪 Lab | `.d64` autostart (LOAD/RUN + warp), both joystick ports, pause/exit menu |
| **ZX Spectrum** | floooh's chips | 🧪 Lab | BIOS from SD, auto-fit screen, configurable GAME/TIME/B mapping |
| **game.com** | Tiger | 🧪 Lab | plays the library; 4-action pad mapped onto G&W buttons |
| **Odyssey² / Videopac** | O2EM | 🧪 Lab (enabled) | raw-ROM path fixed; save/load/resume; multi-game cart select |
| **Super Metroid** | snesrev/sm port | 🧪 Lab | native C reimplementation, 60 fps, savestates. [Details](./super-metroid.md) |
| Tamagotchi | TamaLib | Upstream (P2 🧪) | P1 upstream; P2 experimental in this fork |
| NES, Game Boy / Color, Master System, Game Gear, Genesis, SG-1000 | fceumm / gnuboy / smsplusgx / gwenesis | Upstream | see upstream docs |
| MSX 1/2/2+, Amstrad CPC6128 | blueMSX / caprice32 | Upstream | preview-quality; see upstream docs |
| PC Engine / TG-16, ColecoVision | pce-go / smsplusgx | Upstream | see upstream docs |
| Atari 2600 / 7800, Watara Supervision, Pokémon Mini | stella / prosystem / potator / PokeMini | Upstream | see upstream docs |
| Game & Watch / LCD Games | LCD-Game-Emulator | Upstream | `.gw` files |
| SNES: Zelda 3, Super Mario World | homebrew C ports | Upstream | asset-file build; see upstream docs |
| Celeste Classic | Pico-8 port | Upstream | see upstream docs |
| Pico-8 | macs75 engine | Upstream | separate package install; see upstream docs |

## BIOS files the added systems expect

| System | SD path | Files |
| --- | --- | --- |
| PC Engine CD | `/bios/pce/` | `syscard3.pce` (Super CD-ROM² System Card 3.0; `syscard3.bin` also accepted) |
| ZX Spectrum | `/bios/zxs/` | `48.rom` |
| Commodore 64 | `/bios/c64/` | `kernal.bin`, `basic.bin`, `chargen.bin` |
| Odyssey² / Videopac | `/bios/videopac/` | `o2rom.bin` |
| game.com | `/bios/gamecom/` | `internal.bin`, `external.bin` |

Atari Lynx, WonderSwan, Neo Geo Pocket, Virtual Boy and Game Boy Advance need no BIOS files.
