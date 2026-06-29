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

## The host harnesses

Present in the tree today:

```
linux/pce/   linux/lynx/   linux/o2em/   linux/zx/
linux/a2600/ linux/amstrad/ linux/celeste/ linux/gb-tgbdual/ linux/gwenesis/
linux/msx/   linux/nes/   linux/nes-fceumm/   linux/pkmini/
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

## PC Engine CD / Super CD-ROM² — pce core  ·  [host harness `linux/pce`]  ·  *Ai Chou Aniki* boots to gameplay

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

**State:** MASAYA logo → intro art → Stage 1 gameplay, reproduced on the host
deterministically. **Not upstream-ready yet:** ADPCM/CDDA audio is currently drained and
discarded (no CD sound), save/load needs polish, and on-device confirmation is pending.

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

## Magnavox Odyssey² / Videopac — O2EM core  ·  [host harness `linux/o2em`]

- The core was already in-tree but disabled; enabling it surfaced two latent bugs in the
  previously-uncompiled glue (a `SaveState`/`LoadState` signature mismatch, and a BIOS path
  that didn't match this firmware's `/bios/<sys>/` convention).
- The O2 BIOS waits at a "SELECT GAME" keypad prompt, so a bare launch just sits there. Added
  a small game-select overlay (UP/DOWN pick 0–9, A starts, ~5 s → game 1).
- `linux/o2em` host-verified that "1" + RETURN boots **K.C. Munchkin** to maze gameplay
  (PPM-confirmed) before touching the device.

---

## In bring-up (not in this build)

- **ZX Spectrum** — core vendored (floooh/chips `zx.h`, zlib) and host-validated on
  `linux/zx` for `.z80` loading; integration in progress.
- **Commodore 64** — core vendored (floooh/chips `c64.h`, zlib — chosen over Frodo for
  consistency with the ZX path) and **host-validated on `linux/c64`**:
  - Boots to the real `**** COMMODORE 64 BASIC V2 ****` / `38911 BASIC BYTES FREE` /
    `READY.` screen (proves 6502 + KERNAL RAM test + VIC-II + chargen).
  - A typed BASIC line (`POKE 53280,2 : POKE 53281,5 : PRINT …`) executes — border→red,
    background→green, text printed — proving the keyboard matrix, BASIC interpreter, and
    CIA/VIC register writes (deterministic, screenshot-confirmed).
  - **Device feasibility finding:** `sizeof(c64_t)` is **790 KB**, *over* `RAM_EMU`
    (~724 KB). The cause is the embedded 512 KB datasette (`c1530`) tape buffer. Since the
    initial scope is `.prg`/`.crt` (no `.tap`), shrinking that buffer drops the core to
    ~278 KB — a comfortable fit. This is the gating fact for the device port and was found
    on the host before any flash.
  - Test ROMs (KERNAL/BASIC/chargen) are **never committed** — user-supplied in
    `/bios/c64/`, with MEGA65/open-roms as the documented GPL fallback. See
    `docs/NEW_SYSTEMS_PLAN.md`.
  - **Device integration written and ARM-verified:** `c64_impl.c` (CHIPS_IMPL TU,
    symbols localized except the `c64_*` API) + `main_c64.c` (mirrors `main_zx.c`:
    320×240 crop at framebuffer (100,40), joystick→both ports, `.prg` quickload +
    autostart `RUN`, raw-struct save/load), wired through `Makefile`/`Makefile.common`,
    both linker scripts (`.overlay_c64`/`_bss`), `gw_linker.h`, `rg_emulators.c`
    (dispatch + `add_emulator("Commodore 64","c64","prg",…)`), `MAX_EMULATORS` 25→26.
    **Both C64 objects compile clean for `arm-none-eabi-gcc`, and the `.overlay_c64`
    RAM ASSERT passes** (the dead `c64_load_snapshot` static is GC'd, like ZX).
  - **Full firmware links with C64 in it.** A branch-wide blocker surfaced first — the
    256 KB internal FLASH bank overflowed by ~8 KB once the accumulated per-system glue
    (ZX + C64 + all the rest) was added (proven *not* C64-specific: removing the C64
    `add_emulator` moved the overflow by only 40 bytes). Fix: build the **resident
    firmware `-Os`** (the `build/core/%.o` rule; launcher/HAL/UI are not performance-
    critical — emulation runs in the RAM-overlay cores, which keep their own `-O2`).
    That reclaimed ~27 KB. Result: `gw_retro_go_intflash.bin` = **243,464 / 262,144 bytes
    (92.9 %, ~18 KB free)**, `.overlay_c64` RAM ASSERT passes, `app_main_c64` + its FLASH
    veneer present. This same `-Os` fix also unblocks ZX. Still pending: on-device test
    (real C64 ROMs in `/bios/c64/`), `.crt`/`.d64`, and SID audio.

## Dropped (kept honest)

- **DOOM** and **Wolfenstein 3D** were dropped. For DOOM the RAM zone was *not* the blocker
  (solved at 468 KB) — the wall was XIP-veneer / OSPI 3-region execution corruption, which
  is device-specific and can't be reproduced on a host harness, so the approach that carried
  everything above didn't apply and the ROI didn't justify continuing. Wolf3D's "quits at
  launch" was never root-caused.

---

Related issues: #8 (video player), #9 (debugging post-mortem), #10 (WonderSwan accuracy),
#11 (upstream-contribution tracker).
