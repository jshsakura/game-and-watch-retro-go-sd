---
id: sega-cd
title: Sega CD / Mega CD
---

# Sega CD / Mega CD

PicoDrive Sega CD is included in the SD-card build. Put original `.cue` files
and all their referenced tracks under `/roms/segacd/`. Convert CHD images to
cue/bin offline; the device does not decode CHD.

## Regional BIOS

Supply the 128 KiB BIOS matching the disc's region under `/bios/segacd/`:

| Region | Filename |
| --- | --- |
| US | `bios_CD_U.bin` |
| Europe | `bios_CD_E.bin` |
| Japan | `bios_CD_J.bin` |

The disc determines the region. Korean releases tested in this port used the
Japanese BIOS. Keep disc sector 0 intact: an earlier security-block patch
corrupted Japanese boot code and that diagnosis was withdrawn.

## Tested behavior

The port provides BRAM, savestates, power-off resume, remappable controls and
scaling. The Final Fight CD intro reached about 60 drawn FPS after hot-code
placement at 340 MHz. Other titles and scenes may differ.

Save/load, resume, menu colors, thumbnails and Full scaling were checked on
the device. Off/Fit scaling still needs a visual check. The old RAM-limit
conclusion belongs to the retired gwenesis implementation; the current port
uses PicoDrive with a different memory layout.

[Port worklog](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SEGACD_WORKLOG.md) · [Release status](./release-status.md)
