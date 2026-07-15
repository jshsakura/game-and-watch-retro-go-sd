---
id: overclock-and-power
title: Overclock & power
---

# Overclock & power

The launcher's **CPU Overclock** setting (`PAUSE → Options`) chooses one of three clock levels.
The most important fact for battery: **the core voltage does not change between them.** The
regulator is pinned at the H7's top voltage scale (VOS0) at *every* level, because even "None"
(280 MHz) already needs it — so there is **no V² jump**, and the core's active power rises only
with frequency.

| Level | Core clock | External-flash (OSPI) clock | Core active power vs. None\* |
| --- | --- | --- | --- |
| **None** | 280 MHz | 64 MHz | baseline |
| **Intermediate** | 312 MHz | 104 MHz | ≈ **+11 %** |
| **Maximum** | 340 MHz | ~97 MHz | ≈ **+21 %** |

\* Rough estimate from the clock ratio at fixed voltage — approximate, not a measured figure.

Two things make the **whole-device** impact much smaller than those numbers look:

- They are **core-only.** The LCD backlight, audio, SD card and regulator draw the same at any
  clock, and the backlight is usually the single largest consumer — so the real battery-life
  reduction from overclocking is a *fraction* of the core figure, and your **brightness setting
  matters more than the OC level.**
- Leakage is already paid at VOS0 in every level, so only the switching part grows.

Overclocking also raises the **external-flash (OSPI) clock**, so it costs more on cores that run
code/data straight from external flash (XIP) — [Game Boy Advance](./game-boy-advance.md) (ROM + M4A
mixer) and [Super Metroid](./super-metroid.md) (`sm.xip`) — than on cores that live entirely in
internal RAM.

## These systems overclock even when the setting is "None"

Some cores can't hold full speed at 280 MHz, so they **raise the clock automatically while they
run — regardless of your CPU Overclock setting.** It's a *floor*, not an override: if you chose a
*higher* level, yours wins; the automatic level never clocks you back down.

| System | Automatic level while running | = |
| --- | --- | --- |
| **Game Boy Advance** | Maximum | 340 MHz |
| **Virtual Boy** | Maximum | 340 MHz |
| **WonderSwan / Color** | Intermediate | 312 MHz |

So on these three, expect the overclock power draw **whenever you're playing them, even if you left
the setting at None.** It is **not persisted**: leaving the emulator resets the system and restores
your configured clock, so the menus, the clock app and every other core run at the level you
actually chose. (On the one SD-adapter design that is unstable when overclocked — `SDCARD_HW_OSPI1`
— overclocking is disabled entirely, and these cores run at your chosen level.)

`ENABLE_BOOT_OC=1` (in the release flag set) overclocks only the **boot sequence itself** for a
faster start-up; once the launcher finishes booting it applies your saved setting, so it does not
change your steady-state clock.

:::note
The percentages above are rough, ballpark estimates from the clock ratios — not bench
measurements. Real drain depends on backlight, core and workload.
:::
