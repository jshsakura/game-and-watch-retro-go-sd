---
id: features
title: Apps, launcher & more
---

# Apps, launcher & system-wide changes

## Apps (homebrew overlays)

| App | What it does |
| --- | --- |
| **Music player (MP3)** | minimp3 streaming, album art (HW JPEG + PNG, correct colours), ID3 tags, seek; keeps playing across sleep and while browsing |
| **Video player (MJPEG-AVI)** | faster SD reads, sleep recovery, jitter-buffer read-ahead; companion encoder VBV-caps heavy scenes |
| **Clock** | full clock suite — see below |

## Clock (TIME menu → Clock)

A full-screen clock app benchmarked on the Game & Watch alarm clock, drawn entirely in code so
nothing copyrighted ships.

| Aspect | What |
| --- | --- |
| Modes | Clock · Pomodoro · countdown Timer · Stopwatch (shared layout — `A` start/pause, `B` reset, `PAUSE` = settings incl. Exit) |
| Look | 7-segment / pixel / dot faces × **8 colour themes**; real G&W logo, mode-icon pager, `<>` chevrons, battery, DND moon, localized date/weekday, AM/PM |
| Alarms | set on a full-screen clone of the clock face (edited field blinks); **snooze** (ring → `A` = +5 min, anything else = stop); synthesised beep at system volume; localized |
| Background | off / ambient / built-in pixel scene / animated GIF (`/clock/gif/bg.gif`, must be **320×240**, decoded a frame at a time to RGB565 from the emulator-RAM pool, borrowed and released) |
| Assets | GIFs & icons prepared by the companion [game-and-what](./about.md#companion-game-and-what) tool (`encode_to_clock_gif`: palette-optimized, dithered for RGB565) |
| Config / dev | config in `/clock/clock.cfg`; `host/clock_preview.c` renders pixel-exact PNGs for design review; `tests/` has host unit tests for the alarm logic and GIF pipeline |

## Launcher

| Change | What |
| --- | --- |
| **System-grid home** | all 28 systems on a 6×3 page of rounded tiles (reuses the tab icons — 0 extra resident RAM). Worst case 8 presses to any system instead of 28. Hold `LEFT`/`RIGHT` past the tabs, or `B` from a list; `A` opens. Cold boot → grid, return-from-game → the list you launched from |
| **Favorites tab (★)** | plain-text `/favorites.txt` shown first; 0 resident RAM (shared list buffer). Toggle from the A-button menu; mixed-system covers letterbox into one poster slot |
| **Wordmarks & icons** | per-system name headers in one font/size, 28×28 colour tab icons. NES → `NES (FAMICOM)`, MSX → `MSX / MSX2+` |
| **Carousel wrap** | single-screen lists no longer repeat to fill the view; only multi-page lists connect end-to-start |
| **i18n** | added strings translated across the 12 supported languages; older SD language bins stay compatible |

## System-wide

| Change | What |
| --- | --- |
| **Game caching speed** | ROM flash cache erases with the chip's largest erase command instead of per-4KB; "Caching game" is several times shorter. Fixed a 256KB-sector buffer overflow and a missed erased-tail invalidation along the way |
| **Blit speed** | framebuffer MPU regions are Normal non-cacheable, saving ~1.4-2.1 ms per full-screen blit on every system, with explicit ordering guards |
| **Battery gauge** | filter state persists across power-off in an RTC backup register, with a display limiter and a sleep-entry reference — no seesaw between boots, no stale value after charging asleep |
| **Idle auto-sleep** | an untouched game sleeps the device instead of draining the battery |
| **Sleep recovery** | SD file handles (music, video, PCE-CD) self-heal after the card is power-cycled by sleep |
| **Boot-loop rescue** | watchdog armed from the first line of `main()`; two consecutive failed boots stop the third at a **rescue screen** (boot-to-menu / normal boot / power off) *before* SD, config or auto-resume are touched. POWER on the crash screen really powers off. `TIME` at power-on still skips auto-resume |

## Korean text in the homebrew games

| Game | How Korean is provided |
| --- | --- |
| **Zelda 3** | a `ko` entry in the language menu, from the ZELDA3_K fan translation; full 16×16 glyphs rendered variable-width so syllables come out complete. Needs a `zelda3_assets.dat` built with `--languages ko` (dat + overlay + firmware are a matched set) |
| **Super Mario World** | the level message-box font swaps to Korean through an ExGFX slot, uploaded before and restored after each message |
| **Super Metroid** | the port reads the ROM you supply — a fan-patched ROM's second language (Korean) is selectable from Options. The patch has no 65816 code; the fix was reading 14 text-table pointers back out of the ROM |

Translation data itself is **not** distributed here.
