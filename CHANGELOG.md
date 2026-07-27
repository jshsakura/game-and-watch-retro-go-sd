# Changelog

## What's New

### Version 1.4.0

- Improved SD Card performances
- Add Favorites feature
- Faster ROM flash cache ("Caching game"): erase with the chip's largest sector size instead of 4KB chunks
- Add Atari Lynx support (experimental)
- Add experimental Game Boy Advance support via gpSP (SD card only), sound is choppy, performances will depends on games, requires bios file /bios/gba/gba_bios.bin (open source bios is included but it's not recommended as it could cause some bugs in some games)
- PC Engine :
  - Add PC Engine CD support (beta) (SD Card only not for flash only mod) / requires /bios/pce/syscard3.pce bios file with MD5 = 38179df8f4ac870017db21ebcbf53114
  - Fix Toy Shop Boys not booting (but has small gfx issues)
- MSX :
  - Add support for HDD disk images (R/W) using nextor rom
  - Add NEO16 mapper support
  - Add ASCII-X mapper support (without AmdFlash due to RAM limitation)
- Genesis :
  - Fix gfx issue with International Superstar Soccer Deluxe
  - Add EEPROM support
- Gameboy :
  - Fix various emulation issues (Street Fighter 2, Mr. Do, Prehistorik Man, ...)
  - Add MBC1M mapper
  - Add support for Super Gameboy palette and borders for compatible games (select SGB system in options when game is running, border can be disabled to keep palette feature only)
- Reduced maximum overclocking from 353 to 340MHz to fix some instabilities in Genesis emulator
- Fix issue which could cause battery to drain too quickly while on sleep.
- Fix OC profile not restored after sleep/wake up for MSX/Amstrad/NES/Genesis cores (it was causing slowdowns after power off/power on button press)
- Fix Pico-8 Screenshots (LUT8 mode)
- Reworked language management to limit amount of used RAM
- Reworked NES mappers bin files, it's not included in a single file -> Less space needed in FS, faster firmware update process.
- Added dtcm_calloc/dtcm_malloc to dynamically allocate some info only needed in retro-go frontend so it is available for emulators as it's not needed anymore (allowed to fix pico-8 crash when not using English language)

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
