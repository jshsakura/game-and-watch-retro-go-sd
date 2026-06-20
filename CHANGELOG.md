# Changelog

## What's New

### Version 1.3.2

- Better CJK fonts by using Fusion font
- Genesis emulator various improvements :
  - Add support for 50Hz Megadrive (with region autodetection or manual selection) : fix non US region locked games
  - Various fixes for bad gfx in Top Gear 3, True Lies, Populous, Viewpoint, Bonanza Bros, ...
  - Fixes crash for Shadow of the Beast 1 & 2, Red Zone, Twinkle Tale, Formula One, European Club Soccer, ...
  - Fix Monster World 4 music/sounds
  - Games using EEPROM instead of SRAM aren't supported because there is not enough available space to add EEPROM support logic. Most of these games are still playable by using a SRAM patch to the roms.
  - Compatibility sheet is available here : https://docs.google.com/spreadsheets/d/1v_ysHuK0QrVxto2aKH_08LzrP1QkL8WBJvO4ootC8P0/edit?usp=sharing it gives some information about games patches to use for games using EEPROM
- NES Fceumm emulator : fix for some games not working
- GB emulator : fix for screen transitions showing white instead of correct palette color
- MSX : Add support for YJK graphics (Screen 10-12 MSX2+ modes), due to memory constraints, 256KB YJK table has be reduced to 64KB table which is introducing a small loss of quality compared to original system.
- Add Norvegian translation (Thanks to Idar Lund)
- Various updates needed to improve Pico-8 support, be sure to use https://github.com/Macs75/pico8_gnw_distro/releases/download/v.0.1.6/pico8_cores-2026-06-07.zip or later package
- Preliminar support for flash only systems : compile this project with SD_CARD=0 to be able to install the code from this repository in your G&W without SD Card mod. Documentation will be added soon.

## Prerequisites
To install this version, make sure you have:
- A Game & Watch console with a SD card reader and the [Game & Watch Bootloader](https://github.com/sylverb/game-and-watch-bootloader) installed.
- A micro SD card formatted as FAT32 or exFAT.

## Installation Instructions
1. Download the `retro-go_update.bin` file.
2. Copy the `retro-go_update.bin` file to the root directory of your micro SD card.
3. Insert the micro SD card into your Game & Watch.
4. Turn on the Game & Watch and wait for the installation to complete.

Note : To update bootloader you can download [gnw_bootloader.bin ](https://github.com/sylverb/game-and-watch-bootloader/releases/latest/download/gnw_bootloader.bin) and [gnw_bootloader_0x08032000.bin](https://github.com/sylverb/game-and-watch-bootloader/releases/latest/download/gnw_bootloader_0x08032000.bin) and put them in the root folder of your sd card with `retro-go_update.bin`. After booting the console, the standard update will start and bootloader will also be updated. Check "Bootloader Update Steps" section of README.md for more details, but be aware that a bootloader update failure will require jtag connection to rewrite the bootloader.

## Troubleshooting
Use the [issues page](https://github.com/sylverb/game-and-watch-retro-go-sd/issues) to report any problems.
