# WonderSwan / WonderSwan Color support — draft

These are reference notes for a WonderSwan port to the SD-card build, on top of
the `oswan-go` core that `main` already vendors as a submodule. I'm **not**
opening a PR with this — it lives on the `wswan-upstream` branch for you to test
and pick apart whenever you feel like it.

I know you've been doing your own WonderSwan work on your `wonderswan` branch and
said you'd probably rather finish that than use this — totally fair. This branch
is offered only as reference: if any single piece here (a fix, the pacing math,
the overlay wiring) is useful to you, take it; otherwise ignore it. Nothing here
is meant to steer your own port.

`main` already pins `external/oswan-go` at `efea1326`, so the core is half in the
tree already; what's missing upstream is the front-end glue (main loop, input,
video/audio bridge, savestates, launcher entry). That glue is what this branch
adds, plus 17 core-side commits on top of your `efea1326` pin.

---

## The 60-vs-75 FPS question (the important part)

You flagged: *"WonderSwan is running at 60FPS but it should run at 75fps as on
original system."* Real WonderSwan hardware refreshes at **75.47 Hz**. I audited
our current code path end-to-end (front-end pacing + audio DMA + the core's
per-frame step) to answer the specific question you'd want answered: **is the
emulation genuinely slowed to 60 Hz (~80% speed), or is it full-speed with the
display decimated?**

**Verdict on this branch: it runs at full speed ~75 fps — both emulated
game-time and the physical LCD. It is *not* slowed to 60 Hz, and it is *not*
"75 emulated, decimated to a 60 Hz panel." Both the CPU/game-time pace and the
panel scan-out are driven at 75.**

The 60 fps you observed matches an **older state**. The port's own history
oscillated 75→60→75 (`03497db9` introduced it at 75, `a4b81e8a` dropped to 60 to
stop a frame-pacing runaway that froze the screen, `76b2c972` restored 75 once a
render-skip path let the pacer keep up). If a build in your hands showed 60, it
predates the restore — or the panel rejected the 75 Hz PLL case and silently fell
back to 60 while game-time still ran at 75 (worth checking `lcd_get_last_refresh_rate()`
on device).

Evidence (all on this branch / the ported files):

- **Target FPS is 75, set explicitly**, overriding the common-emu 60 default.
  `Core/Src/porting/wswan/main_wswan.c`:
  `#define WS_FPS (75)`, and in `app_main_wswan()`
  `common_emu_state.frame_time_10us = (uint16_t)(100000 / WS_FPS + 0.5f);`
  (= 1333 → 13.33 ms → 75 Hz) and `lcd_set_refresh_rate(WS_FPS);`.
  The generic default it replaces is `Core/Src/porting/common.c`
  (`100000 / 60` → 60 Hz), which WonderSwan never uses.
- **The panel physically scans at 75 Hz.** `Core/Src/gw_lcd.c`
  `lcd_set_refresh_rate()` has a real `frequency == 75` PLL3 case (`plln=15,
  pllr=32`) that reprograms the LTDC pixel clock — no clamp to 60. Under nominal
  load every emulated frame is both rendered and presented, so it's genuinely
  75 fps end-to-end, not decimated. (This case already exists upstream in `main`.)
- **The hard real-time gate is the audio DMA, and the math yields 75.0 fps.**
  The dominant pace-setter is a per-frame wait at the bottom of the WS loop that
  blocks on exactly one audio-DMA half-buffer tick per emulated frame. One half-
  buffer = `WS_AUDIO_BUFFER_LENGTH = WS_SAMPLE_RATE / WS_FPS = 44100 / 75 = 588`
  mono samples. The SAI outputs those at 44100 Hz mono, so one tick =
  588 / 44100 s = **13.333 ms = 1/75 s exactly**. Waiting one tick per frame
  paces CPU + game-time to 75.00 fps. The buffer math assumes 75, the DMA
  delivers 75, the loop consumes one buffer per frame — self-consistent. If it
  were secretly 60, the buffer would be 735 samples and the tick 1/60 s; it is
  not.
- **Audio pitch is correct independent of the core's internal APU rate.**
  `ws_pcm_submit()` consumes exactly the APU samples produced that frame and
  linear-resamples them to fill the 588-sample buffer, so one emulated frame's
  audio always maps to one 1/75 s output buffer regardless of the core's internal
  sample constants.

**One honest caveat:** the integer `WS_SAMPLE_RATE / WS_FPS = 588` produces
**exactly 75.00 fps**, whereas true hardware is 75.47 Hz — so this is about
**0.6% slow**, a rounding-level discrepancy, categorically different from the
~20% "60 vs 75" gap. Hitting 75.47 exactly would need ~584.4 samples/frame at
44100 (or a slightly higher SAI rate), which the integer divide can't express.
If you care about the last 0.6%, that's the knob.

There's also a core-side timing fix in the bump (`437a489`): the core's `WsRun`
was advancing **1.34 frames per call** (audio/game ran fast) and was corrected to
exactly one frame (1272 cycles). If your `efea1326`-based build ran fast rather
than slow, that commit is likely the relevant one.

---

## What it does

- Adds a "WonderSwan" system that lists `*.ws` / `*.wsc` ROMs from `/roms/ws`.
- Brings up the `oswan-go` core (NEC V30MZ CPU, APU, renderer) in its own RAM
  overlay (`.overlay_wswan`, streamed from `/cores/wswan.bin`), exactly like the
  Watara Supervision overlay it sits next to.
- Full-speed 75 Hz frame + audio pacing (see above), FIT scaling by default,
  bilinear filter support, savestates, and flash-XIP ROM loading (the ROM stays
  in flash, no RAM copy).

## Files changed and why

Port (the substance):

- `Core/Src/porting/wswan/main_wswan.c` — the front-end: main loop, input map,
  video blit into the shared frame, the ADPCM/APU → SAI audio bridge with the
  75 Hz DMA pacing described above, savestate load/save, and CPU auto-boost.
- `Core/Src/porting/wswan/nec.c` / `necinstr.h` — V30MZ CPU glue for the G&W
  build.
- `Core/Src/porting/wswan/ws_fileio.c` — flash-XIP ROM loader + file I/O shim so
  the core reads the ROM straight from QSPI flash.
- `Core/Inc/porting/wswan/main_wswan.h` — the one-line entry-point header.
- `wswan_redefines` — objcopy `--redefine-syms` list (Page/Noise/graphics_paint)
  so the core's generic symbol names don't clash with other cores at link, the
  same trick smw/zelda3/ngp use.
- `Core/Inc/gw_linker.h`, `STM32H7B0VBTx_FLASH.ld`, `STM32H7B0VBTx_SDCARD.ld` —
  the `.overlay_wswan` / `.overlay_wswan_bss` sections and their extern symbols,
  mirroring the WSV overlay block byte-for-byte (same `__RAM_EMU_START__` base,
  same BSS-overflow ASSERT).
- `Core/Src/porting/common.c` / `common.h` — a 6-line `common_emu_auto_oc(level)`
  helper (calls `SystemClock_Config`, skips the boost on OSPI1 SD hardware). The
  port calls it at startup; upstream cores open-code `SystemClock_Config(2)`, so
  this is just a small shared convenience. If you'd rather not add it, the one
  call site can inline the upstream idiom instead.
- `Makefile` / `Makefile.common` — `WSWAN_C_SOURCES` + includes, the
  `wswan_obj_prereq_gen` build rule (`-O3` + redefine step, like the ngp/smw
  rules), the object list in the link line, the `mkdir build/wswan`, the
  `-j .overlay_wswan` in the extflash objcopy, and the
  `EXTRACT_INTERNAL_CORE_BIN_WITH_HEADER … /cores/wswan.bin` line.

Launcher glue — **PROVISIONAL, please treat as throwaway:**

- `Core/Src/retro-go/rg_emulators.c` — registers the "WonderSwan" emulator,
  dispatches it to the overlay, and bumps `MAX_EMULATORS` 20→21. **Every edit in
  this file is a minimal shim so the branch runs standalone.** Specifically:
  - The emulator is registered and `MAX_EMULATORS` is bumped **only under
    `#if SD_CARD == 1`**, mirroring the DTCM-tight convention pcecd-upstream
    used (the `emulators[]`/`systems[]` arrays live in DTCM `.bss`, so the bump
    only happens where the entry is actually compiled in). WonderSwan is **not**
    inherently SD-only — the ROMs are small and flash-XIP'd — so if you want it
    on non-SD builds too, just drop the `#if SD_CARD` guards; the overlay itself
    builds fine either way. I left the gate on purely to match the existing
    convention and keep the non-SD DTCM budget untouched.
  - **Logo:** this branch ships **no fork-custom logo blobs**. There is no
    WonderSwan logo upstream, and `RG_LOGO_PAD_HOMEBREW` doesn't exist, so the
    entry reuses the generic Game Boy handheld logos (`RG_LOGO_PAD_GB` /
    `RG_LOGO_HEADER_GB`) as a neutral placeholder. Swap in a real WonderSwan
    logo however you handle art.

## Submodule handling (needs your hand)

`main` already pins `external/oswan-go` at `efea1326` (your commit). This branch
bumps that pin to `beabf6ea`, which is a **clean fast-forward** — 17 commits
linear on top of `efea1326`, touching only the core:

- `f2f2a17` flash-XIP ROM loader for the G&W port
- `4a6cc74` / `1aac122` / `c70cca8` GNW_WSWAN build guards (skip SDL/dirent for
  bare-metal)
- `b27a3b4` anchor ROM banks to end of image (match `WsCreate` SEEK_END)
- `6b51b63` `ws_render_enabled` — skip per-scanline render on dropped frames
  (this is what lets the 75 Hz pacer keep up)
- `7e1b9ba` shrink APU noise tables + ring to fit `RAM_EMU` under `SOUND_ON`
- `66c4346` / `fa8f3d1` memory-based savestate + file-direct savestate, back all
  SRAM banks
- `a2dcf44` / `a6edcd9` / `9d4dfd3` V30MZ perf (inline `ReadMem`, direct-branch
  IRAM writes, cache `CS<<4` in the fetch path) — headroom for 75 fps
- `caa6811` savestate header rejects incompatible/old saves
- `d3f8cfd` replay display regs on load (tilemap base was stale → garbled screen)
- `437a489` **`WsRun` = exactly one frame (1272 cycles)**, was 1.34 → game/audio
  ran fast (the timing fix mentioned above)
- `beabf6e` **handle sound DMA (port 0x52)** to prevent a boot hang — this is the
  One Piece "Grand Battle" boot-voice poll-loop fix.

The main-repo PR can only pin a submodule SHA, so for you to build/review this
that commit has to be reachable. These 17 live on a branch in a fork of
`oswan-go`; I can push it, or you may prefer a small separate PR against
`oswan-go` first so the core diff is reviewed on its own. It's a clean
fast-forward of core-only changes over the exact commit `main` already pins.

## Tested titles (from fork history, on device)

WonderSwan mono and Color titles boot, run, save/load, and sleep/wake. One Piece:
Grand Battle — Swan Colosseum was the title that surfaced the port-0x52 sound-DMA
boot-hang fix (it froze at a boot voice poll-loop on unimplemented port 0x52).

## Open questions for you

1. **SD gate.** I gated the launcher entry on `SD_CARD=1` to match pcecd's DTCM
   convention, but WonderSwan doesn't need SD. Want it on all builds?
2. **75.47 vs 75.00.** Worth chasing the last 0.6% (non-integer samples/frame or
   a nudged SAI rate), or is exact-75 fine?
3. **auto_oc helper.** OK to add the small `common_emu_auto_oc` to `common.c`, or
   would you rather the one call site inline `SystemClock_Config`?
4. **Logo/naming.** Placeholder GB logo + the plain name "WonderSwan" (no
   separate "WonderSwan Color" entry — the core auto-detects). Adjust to taste.

## Build status (honest)

- **WonderSwan objects + launcher glue compile clean** with the local toolchain
  (`SD_CARD=1 COVERFLOW=1 INTFLASH_BANK=2 CHECK_DIRTY_SUBMODULE=0`,
  `CFLAGS_EXTRA="-D__int64_t_defined=1 -DPATH_MAX=256"`): all six core/port
  objects (`WS`, `WSApu`, `WSRender`, `main_wswan`, `nec`, `ws_fileio`), plus
  `rg_emulators.o` and `common.o`, build with zero warnings/errors.
- A **full-firmware ELF link could not be completed in this environment**, but
  **not because of WonderSwan**: three unrelated cores that `main` pins
  (`fceumm-go e5b2d44e`, `caprice32-go dc3ecc29`, `LCD-Game-Emulator e2c2ed6c`)
  reference submodule commits that are no longer reachable on their forks (not on
  any branch or tag), so those cores can't be checked out to the pinned layout
  and the link stops there. On your CI, where those pins resolve, the WonderSwan
  overlay links exactly like WSV. The `SD_CARD=0` build hits the same unrelated
  wall first, and the `main` base additionally has a known DTCM overflow with
  this particular local toolchain — neither is WonderSwan-related.
