---
id: build-flags
title: Build-time feature flags
---

# Build-time feature flags (env)

Beyond choosing which firmware to flash, what gets *compiled in* is controlled by make/env flags.
The canonical release set (from `.github/workflows/package.yml`) is:

```bash
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
             ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

| Flag | Default | Toggles |
| --- | --- | --- |
| `COVERFLOW` | 0 | cover-art carousel views |
| `CHEAT_CODES` | 0 | Game Genie / cheat support (GB, GBC, NES, PCE, MSX) |
| `ENABLE_BOOT_OC` | 0 | overclock at boot |
| `ENABLE_SCREENSHOT` | 1 | `PAUSE`+`GAME` screenshot capture |
| `SHARED_HIBERNATE_SAVESTATE` | 0 | separate savestate for off/on hibernate |
| `DISABLE_SPLASH_SCREEN` | 0 | skip the startup splash animation |
| `ZH_CN` `ZH_TW` `KO_KR` `JA_JP` `RU_RU` `FR_FR` … | varies | per-language UI + fonts on SD `/lang` and `/fonts` |
| `GNW_TARGET` | mario | `mario` / `zelda` button mapping & default extflash size |
| `INTFLASH_BANK` | 2 | which internal-flash bank to link into (dual-boot = 2) |
| `SD_CARD` | 1 | SD-card variant (`0` = the all-in-flash build — different link script & feature set) |

Run `make help` for the authoritative, current list. Note the lab apps (grid home, favorites,
clock, media players) are always compiled in — they have no on/off flag.

For the full build/flash workflow, Docker image and toolchain, see the
[upstream README](https://github.com/sylverb/game-and-watch-retro-go-sd) and the repository's
`CLAUDE.md`.
