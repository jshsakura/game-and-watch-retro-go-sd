# Host-harness engineering notes

> Consolidated record of how the testbed features were actually debugged, written so the
> fixes can be reviewed on their merits before any are offered to
> [`sylverb/game-and-watch-retro-go-sd`](https://github.com/sylverb/game-and-watch-retro-go-sd).
> Companion to the upstream-contribution tracker (issue #11).

## A note on who wrote this and how

I don't come from an embedded background, and I'm not fluent with JTAG single-stepping or a
hardware debugger. So I leaned on the two things I could actually control:

1. **Host harnesses** — small Linux/SDL programs under `linux/<sys>/` that link the *same
   core source* the firmware ships, run headless and deterministically, and dump frames or
   logs. Where one of these exists, the root cause was proven on a PC before flashing.
2. **SD-card logs on the real device** — for the cores without a host harness, "tested on
   hardware" literally means the firmware wrote a log to the SD card and I read it back,
   changing one thing at a time. No debugger — `printf`-to-SD and iteration.

That's the honest toolchain. Below I mark each item **[host harness]** or **[on-device,
SD-log]** because the confidence level differs and I'd rather you know which is which than
overstate it.

**Where things stand today:** every system below runs on real hardware. The one known
wall is **Virtual Boy's speed** (an interpreter ceiling, ~65-70 %), plus a single
WonderSwan title's mid-battle resume (a cycle-accuracy limit, documented in its section).
Everything else that reads like a battle report below ended in a fix that shipped.

## The host harnesses

Present in the tree today:

```
linux/pce/   linux/lynx/   linux/o2em/   linux/zx/   linux/c64-frodo/
linux/gamecom/  linux/vb/   linux/a2600/ linux/amstrad/ linux/celeste/
linux/gb-tgbdual/ linux/gwenesis/ linux/msx/ linux/nes/ linux/nes-fceumm/ linux/pkmini/
```

Each links the real core (not a re-implementation), runs as
`SDL_VIDEODRIVER=dummy ./build/<core>.elf <frames>`, and produces a `.ppm` and/or an
appended text log. The Lynx harness builds under AddressSanitizer, which turns
random-looking device HardFaults into a precise file:line.

There is **no** host harness for the RACE (Neo Geo Pocket) or oswan (WonderSwan) cores —
those were debugged on the device from SD logs, and I've marked them as such.

---

## Neo Geo Pocket / Color — RACE core  ·  [on-device, SD-log]

- Boots, input, sound, 4-slot savestates, full speed.
- **Savestate sound bug:** after loading a state the sound was dead. The snapshot didn't
  include `ngpRunning` and the Z80 / IRQ chain state, so on load the audio path was never
  re-armed. Fix: re-arm the Z80 / IRQ chain on load and snapshot the missing flag. Found by
  comparing save-vs-load behaviour from SD logs on the device.
- **Display:** fixed FIT / letterbox scaling corruption and a flickering bottom band;
  scaling now follows the global setting and defaults to FIT on first run.

## WonderSwan / Color — oswan core  ·  [on-device, SD-log]  ·  incl. *One Piece: Grand Battle* save/load

- **V30 `0x0F` instruction group + `REPNC`/`REPC`** implemented — this is what gets the
  8 MB *One Piece: Grand Battle / Swan Colosseum* cart booting at full speed.
- **Sound-DMA boot hang / frozen screen:** port `0x52` (a boot-time voice poll-loop) was
  unimplemented, so the title froze before the menu.
- **Save / load (Grand Battle), the part you asked about:** 8 MB carts HardFaulted on
  savestate and on resume. Fixes: mirror the cart banks across the address space, force
  `CS=0` during the `WriteIO(0xC0)` bank replay, correct the `INT 1` stack handling, and
  save/restore the frame-timing phase in the snapshot. That made save and load work.
- **Honest ceiling:** the full *mid-battle resume* case was traced — from SD logs and
  savestate diffs — to an emulator **cycle-accuracy / IRQ-timing** limit in the interpreter,
  not a bug I could patch at this layer. I'm flagging that rather than claiming it's solved
  (#10). The CPU optimizations applied are minimal-diff and meant to be mergeable.

## PC Engine CD / Super CD-ROM² — pce core  ·  [host harness `linux/pce` + on-device]  ·  verified on device, four titles played through

The longest campaign: ~30 commits, iterations `it2`→`it27` on `feat/pcecd`. The harness is
`linux/pce`, writing `/pcecd_diag.txt`, a CPU-PC ring sampler, and frame PPMs.

Build-up (it2–it13): ported a SCSI target from Mednafen `pce_fast`, wired the CUE/BIN disc
layer (TOC parser + sector reads), then iterated the data-in lifecycle — manual-ACK for the
TOC vs. the bulk-READ loop, routing the bulk READ through the `$1808` auto-increment port
with `$1801` auto-ack. Each step was a logged SCSI register trace.

Two root causes, both found on the host trace and fixed in shared device files:

1. **`$18C0`–`$18C7` Super System Card signature** (`EX_MEMOPEN $E0DE`). The game reads it
   to confirm Super System Card RAM is present; it was being aliased onto the SCSI register
   block, the game read a non-`$AA` value, decided the hardware was wrong, and **halted at
   `0x6257`**. Fix: the core returns `$18C1=$AA / $18C2=$55 / $18C3=$03`.
2. **ADPCM-from-CD DMA (`$180B` bit 1)** never consumed the bulk READ, so the read never
   finished and the title **looped the boot**. Fix in `pce_scsi.c`: `adpcm_dma_drain()`
   consumes the sectors, signals `DATA_DONE`, advances to STATUS.

Also backed the CD-ROM² program RAM (banks `0x68`–`0x87`) and the full **256 KB** Super CD
RAM with real memory; the savestate includes that 256 KB.

**Dead-end I'll own:** an earlier theory — "vblank never calls the game's user hook" — was
wrong. The PC sampler had been armed at `0x6254`, not the real IPL exec entry `0x6000`; once
the indexing was fixed the actual halt path appeared. The log disagreed with the theory, so
the theory lost.

The campaign continued well past that state, all on the same harness-first method:

- **CD audio, both kinds.** CD-DA (Red Book BGM) streams with batched SD sector reads;
  ADPCM (MSM5205) streams from CD. The ADPCM pipe carried a **2-byte record shift** —
  the drain lost the first byte and the `$180A` half-word latch semantics were missing —
  which desynchronized the `AD_READ` records into garbage block counts and stray I/O.
  Found by diffing the host trace against Mednafen's, fixed at the latch.
- **The "freezes after load" saga** (nine distinct root causes over two days, each one
  proven in a cold-resume host harness before flashing — `PCE_COLD_RESUME=1` replays a
  savestate into a *fresh* process): stale `frame_integrator`, two savestate-section bugs
  (`'ADPC'`, `'SCSX'`), a staging-bank clobber, a missing ADPCM-END IRQ, SUBQ needing
  real data, CD-DA `PAUSED` state surviving save/load, boot-resume `start_paused`, and —
  the on-device finale — the launcher's autostart `RUN` injection un-pausing into a pause.
- **Dynastic Hero's two gates.** First death at boot (`f809`): a PSG timer tick landed in
  a 68-byte window and took a BRK through an unmapped vector — fixed with an h6280
  **BRK-vector guard**. The second gate was the ADPCM 2-byte shift above. Both fixes were
  regression-checked against three other titles before shipping.
- **BRAM saves** persist to SD; savestate/resume covers mid-ADPCM and CD-DA state and the
  full 256 KB Super CD RAM.

**State: done and verified on hardware.** Four titles played through on the device
(Dracula X: Rondo of Blood, Cotton, Dynastic Hero, Ai Chou Aniki), with BGM, ADPCM voice,
BRAM saves and mid-game resume. The cold-resume self-test harness stays in `linux/pce`.

## Atari Lynx — Handy core  ·  [host harness `linux/lynx` + ASan]  ·  verified on device

~35 commits. Harness: `linux/lynx` plus a fresh-construction save/load round-trip harness,
runnable under AddressSanitizer.

- **Two crashes the ASan host harness caught** (random faults on device, precise on host): a
  **BS93 big-endian cart-header parse** bug and a **Mikey render-line bounds** overrun.
- **Boot watchdog reset:** the core hit `UpdateSound` with an ~8.5-million-sample loop and
  blew the watchdog. Fix: clamp the sample count in handy-go + kick the watchdog inside
  `UpdateFrame`. (Related: a stray heartbeat `printf` in the hot path itself crashed
  `UpdateFrame`; the cores now run a printf-free main loop.)
- **Save/load (the hard one):** the menu Save/Load handlers run in **firmware context**,
  where the overlay's `lynx` pointer reads back as **0**. I chased several wrong theories
  (BSS clobber, framebuffer overdraw, firmware-DTCM stashing). What ended it was **one
  3-way SD-log line** distinguishing *pointer-null* vs. *fopen-null* vs. *OK* — it was the
  pointer. Fix: the handler only records `s_pending_save/load` + path; the real
  `ContextSave/Load` runs in the **main loop** next to `UpdateFrame`, where the pointer is
  valid (the same deferred pattern resume-load already used).
- **512 KB carts:** `getromdata` RAM-copies bank 0 only when `heap_free ≥ size + 192 K`,
  else passes the flash pointer so the cart XIPs bank 0 from QSPI (only the 64 K bank 1
  stays in RAM). A "flash-XIP is too slow" assumption I'd been carrying turned out to be
  unmeasured and false.
- **State:** boots, runs, save/load, and 512 KB carts all confirmed on real hardware.

## Magnavox Odyssey² / Videopac — O2EM core  ·  [host harness `linux/o2em` + on-device SD-log]  ·  verified on device

- The core was already in-tree but disabled; enabling it surfaced two latent bugs in the
  previously-uncompiled glue (a `SaveState`/`LoadState` signature mismatch, and a BIOS path
  that didn't match this firmware's `/bios/<sys>/` convention).
- The O2 BIOS waits at a "SELECT GAME" keypad prompt, so a bare launch just sits there. Added
  a small game-select overlay (UP/DOWN pick 0–9, A starts, ~5 s → game 1) — the console has
  a keypad the G&W doesn't, so the missing keys became a UI affordance instead.
- `linux/o2em` host-verified that "1" + RETURN boots **K.C. Munchkin** to maze gameplay
  (PPM-confirmed) before touching the device.
- **Device-only bug the harness could never see:** on hardware every cart sat at "SELECT
  GAME" as if empty. `/videopac_diag.txt` showed the raw-ROM branch of `getromdata`
  returning an overlay-NULL `ROM_DATA` symbol instead of `rom_file->address` — an empty
  cart, so the BIOS had nothing to select. The harness bypasses `getromdata` entirely,
  which is exactly why this one had to be caught by SD log. Runs with save/load/resume.

## ZX Spectrum — floooh `chips` core  ·  [host harness `linux/zx` + on-device SD-log]  ·  verified on device

- Core vendored from floooh/chips (`zx.h`, zlib licence); host-validated for `.z80`
  loading before integration.
- On device: a PAUSE-menu HardFault fixed, the screen auto-fits, and a graceful
  message replaces the crash when `/bios/zxs/48.rom` is missing.
- **The keyboard problem, solved in settings:** Spectrum games expect a 40-key keyboard
  the G&W doesn't have, and every title binds differently. Instead of hardcoding one
  layout, the PAUSE menu exposes **configurable GAME / TIME / B key mappings** — an
  SD-logged trace of every button edge and the ZX key it sends was used to verify the
  path. Keyboard-driven games are playable this way.

## Commodore 64 — Frodo core  ·  [host harness `linux/c64-frodo` + on-device SD-log]  ·  verified on device

Two cores were tried, and the switch is worth recording honestly:

- **First attempt — floooh `chips` `c64.h`** (`linux/c64`): host-validated to the BASIC
  READY prompt with working keyboard/VIC/CIA, and the device feasibility finding
  (`sizeof(c64_t)` = 790 KB, dominated by a 512 KB datasette buffer that shrinks away)
  still stands. But the games library lives on **`.d64` disk images**, and `chips` has no
  1541 drive emulation — the honest scope check said the port would ship unable to load
  most real software.
- **Second attempt — Frodo** (Christian Bauer's emulator, with its full 1541 implementation)
  is what shipped. `.d64` loading works with **autostart injection** (`LOAD"*",8,1` typed at
  the right boot moment, `RUN` after the directory settles, warp during the load) — the
  timing of that injection was itself a regression story: a "fix" based on a wrong diagnosis
  once broke a provably-working baseline and was reverted on the log evidence.
- **A reusable C++-overlay lesson:** Frodo is C++, and its global constructors leaked into
  the resident `.init_array`, hard-faulting the boot before anything ran (black screen).
  Overlay cores must KEEP their `.init_array` inside the overlay and run it on overlay
  entry. Documented because it applies to any future C++ core.
- **Controls:** both joystick ports are mapped (games disagree about which port the player
  is on), plus a pause/exit menu. One SID variant was tried and reverted — it OOM'd the
  heap on device; the shipped configuration is the one that fits.

## game.com (Tiger) — SM8500/SM8521 core  ·  [host harness `linux/gamecom` + on-device SD-log]  ·  verified on device

- Host harness plays Frogger, Sonic Jam, Indy 500 before any flash.
- The handheld has a **4-action pad** (A/B/C/D); the G&W's fewer buttons are mapped so the
  common two-button games feel right and the rest stay reachable.
- What the harness *couldn't* catch became its own documented lesson: cores that run their
  own blocking main loop (this one, and Frodo) must call `audio_start_playing()` before the
  first `sound_sync` (else the first frame hangs — a marathon of a hang to find), pass a
  non-NULL repaint callback to the input loop (else the menu crashes with PC=0), and wire
  the standard input loop for menu/volume/power. The host harness stubs the firmware, so
  all three were found on device via SD log.

## Virtual Boy — red-viper core  ·  [host bench `linux/vb` + on-device]  ·  runs at ~65-70 % speed, stated plainly

- The interpreter was benched on the host for 20k frames clean before porting.
- On device it runs at **about 65-70 % of full speed** with automatic overclock — an
  interpreter on this MCU has a ceiling, and it is stated rather than hidden. Several
  titles are enjoyable at that speed (Wario Land, Tennis); audio is gapless.
- **Controls, solved in settings:** the Virtual Boy has TWO d-pads and the G&W has one, so
  no single mapping is right. The options menu offers **three pad presets** (left pad /
  right pad / triggers-as-A/B), and the Zelda-style X/Y buttons always serve the
  L/R triggers. A replay-buffer OOM was also fixed along the way.

---

## System-wide findings  ·  [on-device unless noted]

Not tied to one core, found while living with the device:

- **Framebuffer MPU regions** were `TEX=0/C=0/B=0` (Strongly-Ordered), stalling every
  store until the AXI write completed — ~1.4-2.1 ms per full-screen blit, on every
  system. `TEX=1/C=0/B=0` (Normal non-cacheable) keeps the same no-cache coherence and
  lets stores merge. The speedup exposed two ordering assumptions, both guarded: a
  `__DSB()` before the LTDC vblank flip, and per-frame clears waiting out a pending flip
  (skipping that guard produced a transient rising black band on cores that clear every
  frame — the bug was observed, understood, and is what makes the guard non-optional).
- **ROM flash cache**: `circular_flash_write()` erased 4 KB per 4 KB of file — 2048 erase
  commands for an 8 MB ROM. Letting `OSPI_Erase()` use the chip's largest erase command
  makes "Caching game" several times faster; two latent bugs (a 256 KB-sector Spansion
  buffer overflow, a missed invalidation of the erased tail) fell out of the same read.
- **Battery gauge**: the filter state lived in RAM, so every power-off reseeded it from a
  fresh load-free reading and the shown percent seesawed (optimistic at boot, plunging
  under game load). It now persists in an RTC backup register with a time-based display
  limiter.
- **Clock after a full drain**: a drain wipes the RTC backup domain and the clock used to
  restart at 2000-01-01, silently. An 8-byte time snapshot written to SD on every
  sleep/power-off entry (all drain paths pass through it, including the 1-5 % auto
  power-off) restores the clock at boot when the wipe is detected. Honest limit: the true
  elapsed downtime is unknowable offline, so the restored clock is *behind by the
  downtime* — approximate on purpose, better than 26 years wrong.
- **Video playback smoothing**: MJPEG frame sizes are bursty; the player now prefetches
  frames into a jitter buffer during each frame's pacing wait, and the companion encoder
  VBV-caps per-frame bytes (measured: easy scenes byte-identical, worst case ~26 % lighter).

## Dropped (kept honest)

- **DOOM** and **Wolfenstein 3D** were dropped. For DOOM the RAM zone was *not* the blocker
  (solved at 468 KB) — the wall was XIP-veneer / OSPI 3-region execution corruption, which
  is device-specific and can't be reproduced on a host harness, so the approach that carried
  everything above didn't apply and the ROI didn't justify continuing. Wolf3D's "quits at
  launch" was never root-caused.

---

Related issues: #8 (video player), #9 (debugging post-mortem), #10 (WonderSwan accuracy),
#11 (upstream-contribution tracker).
