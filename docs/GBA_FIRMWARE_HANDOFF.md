# GBA (gpSP) — firmware handoff

*(`gba_idle_loop.c` and `gba_idle_loop.h` point here.)*

Everything a maintainer needs to carry the Game Boy Advance core into another tree: what
the port is made of, what has to be wired for it to work, the invariants that break
*silently* if they are missed, and where the gpSP fork stands relative to libretro.

Companion documents, none of which this one repeats:

- [`Core/Src/porting/gba/CLAUDE.md`](../Core/Src/porting/gba/CLAUDE.md) — the debugging
  playbook: which rig answers which question, the two idle-skip semantics, the sound path
  end to end, and the traps with scars attached.
- [`tools/gba_m4a/README.md`](../tools/gba_m4a/README.md) — the M4A mixer HLE and every
  knob on the prover.
- [the README](../README.md)'s GBA section — the user-facing write-up (why the ROM never
  enters RAM, the idle table, the M4A story, the memory map, the honest caveats).

---

## 1. What the port is made of

**In the firmware tree** (these are what make the core exist):

| path | |
| --- | --- |
| `Core/Src/porting/gba/main_gba.c` | the porting layer: frame loop, XIP cart setup and the load-time sentinel pass, input, savestates, options, the AHB-SRAM zeroing, `vcount_polls[]` |
| `Core/Src/porting/gba/gba_frontend.c` | the gpSP-side callbacks the core calls back into |
| `Core/Src/porting/gba/gba_idle_loop.c` | 131 measured idle-loop addresses — **generated**, whole-file copy only |
| `Core/Src/porting/gba/gba_audio_filter.c` | the rate-following output low-pass (Padé rational, not libm — see invariant #6) |
| `Core/Src/porting/gba/gba_bios.S` | the clean-room BIOS replacement, `.incbin`'d |
| `Core/Inc/porting/gba/*.h` | the three headers, incl. the savestate ABI |
| `tools/gba_m4a/m4a_hle.c`, `m4a_gpsp.c`, `m4a_hle.h` | the M4A mixer HLE — six transliterations, XIP'd out of flash |
| `gba_redefines` | the symbol namespace (invariant #3) |
| `external/gpsp` @ `9cab230` | the core, branch `gnw-port` — see §3 |

**Wiring, in files shared with every other core:** `Makefile.common` (`GBA_*_SOURCES`,
`C_DEFS_GBA`, the per-object rules, the `gba.bin` / `gba.xip` extraction),
`STM32H7B0VBTx_SDCARD.ld` (the overlay, the ITCM section, `.xip_gba`, `.rodata_gba`,
`.gba_ahbram`), `Core/Inc/gw_linker.h`, `Core/Inc/retro-go/appid.h` (`APPID_GBA = 24`),
`Core/Src/retro-go/rg_emulators.c` (`emu_gba`, `add_emulator(...)`),
`Core/Inc/retro-go/bitmaps.h` + `Core/Src/retro-go/rg_logos.c` (header art and pad icon).

**The rigs** (nothing needs them to run; each exists because something shipped broken
without it):

| path | what it proves |
| --- | --- |
| `tests/test_gba_xip_contract.sh` | the XIP split still holds — the one that catches a **green build that cannot boot** (invariant #1) |
| `tests/test_gba_m4a_wired.sh` | the M4A HLE is really linked, and each piece is on its side |
| `tests/test_gba_audio_filter.c` | the low-pass does what its header promises — passband, stopband, bypass, stability |
| `tools/gba_harness/` | gpSP's real `load_gamepak()` on a host address space with page 0 unmapped |
| `tools/gba_m4a/prove.sh`, `prove_main.c` | the mixer HLE is bit-exact, including guest time; also the general GBA investigation rig (`IDLE_PC=`, `M4A_AUDIO_RAW=`, `IDLE_TRACE=1`) |
| `tools/gba_m4a/census.py` | which carts carry which mixer |

They are standalone — no shared fixtures, no sourcing, each exits non-zero on failure, so
they drop into a tree with no test suite:

```sh
ELF=build/gw_retro_go.elf bash tests/test_gba_xip_contract.sh
ELF=build/gw_retro_go.elf bash tests/test_gba_m4a_wired.sh
cc -O2 -std=gnu11 tests/test_gba_audio_filter.c \
   Core/Src/porting/gba/gba_audio_filter.c -lm -o /tmp/t && /tmp/t
bash tools/gba_harness/run.sh
```

The two ELF tests want `arm-none-eabi-nm` / `objcopy`; with no toolchain, no ELF, or an
`SD_CARD=0` build they **SKIP loudly** instead of failing. That is deliberate: a safety net
that reddens CI over its own missing prerequisites teaches people to ignore CI.

---

## 2. The invariants that break silently

Each of these has already cost a release. Listed in the order they will bite.

**1. The XIP split is a contract the compiler cannot check.**
`cpu.o` — gpSP's ARM7 interpreter — runs from ITCM. The load-time sentinel pass in
`main_gba.c` walks the RAM overlay and rewrites every `0xDEC0xxxx` word to wherever the
flash blob actually landed; **it does not walk ITCM.** So nothing `cpu.o` references may
live in the blob, or the device jumps to `0xDEC0xxxx` on the first frame. Today that holds
because `cpu.o` calls only `gba_memory.o`, `main.o`, `cheats.o`, `savestate.o` and reads no
rodata but its own, and the linker script keeps exactly those in RAM — but the split is *a
list of filenames in a linker script*. Add a source file to the wrong list, or let gpSP
grow a call from `cpu.cc` into `video.cc`, and the build stays green while the device stops
booting. `test_gba_xip_contract.sh` counts the sentinels: `0` in `.overlay_gba_itc`, `1` in
`main_gba.o`'s window, `>0` in the rest of `.overlay_gba`.

**2. `.gba_ahbram` sits outside `.overlay_gba_bss`.**
The BIOS image, the cheat table and the sound ring live in AHB SRAM in their own NOLOAD
section — so `run_internal_emu()`'s overlay memset never reaches them. **`main_gba.c`
zeroes that range by hand** (`memset(__gba_ahb_start__, 0, __gba_ahb_end__ -
__gba_ahb_start__)`, `main_gba.c:731`). Delete that line and the first launch after a reset
works while the *second* inherits the first game's state.

**3. Every GBA object must go through `--redefine-syms=gba_redefines`.**
Cores are overlays linked at the same RAM address, so a symbol a core fails to define does
not fail the link — the linker binds it, quietly, to another core's address, which once the
core is loaded holds unrelated data. The rename makes a missing definition a link error
instead of an alias. Both object rules in `Makefile.common` apply it, and
`scripts/check_core_symbol_aliases.py` fails the build if any core reaches into another's
overlay.

**4. `APPID_GBA = 24` is a number, not a label.**
`/CONFIG` is a raw dump of `persistent_config_t`, which contains `app[APPID_COUNT]`.
Renumbering or inserting an APPID grows the struct, the magic check throws the saved file
away, and **every user loses language, coverflow, backlight and volume.**

**5. SD-card builds only.**
`GBA_OBJECTS` is empty unless `SD_CARD=1`. A FrogFS build can neither cache and relocate
`gba.xip` nor hold the up-to-32 MB cart the core reads straight out of flash, and the
sections exist only in `STM32H7B0VBTx_SDCARD.ld`. Porting to the non-SD tree means
answering the cart-storage question first, not copying linker sections.

**6. libm is resident, and the audio filter is why.**
libm lands in resident internal flash. A single `tanf()` in the filter once overflowed
internal flash by 1,412 bytes; `gba_audio_filter.c` computes its cutoff with a Padé
rational on purpose. Do not "simplify" it back to libm.

**7. One sample rate, one definition.**
`GBA_SOUND_FREQUENCY=48000` is the SAI's rate, so nothing is resampled. The PSG frequency
steps derive from that single definition and are `_Static_assert`-pinned — they were
duplicated across two files once, and every note-on came out 5.39 semitones flat.

---

## 3. The gpSP fork

- **Fork:** `jshsakura/gpsp`, branch **`gnw-port`**, pinned at `9cab230`.
  `sylverb/gpsp` is a fork of it, so the pinned SHA resolves from either; `.gitmodules`
  currently points at `jshsakura/gpsp.git`, and repointing it is a one-line decision.
- **Base:** libretro/gpsp `69e86eb`. The G&W port is **13 commits** on top:

  | | |
  | --- | --- |
  | `aa160f0` | fit the interpreter in a microcontroller's RAM budget |
  | `a21fcc9` | run the cart straight out of memory-mapped flash |
  | `7553ff7` | a savestate that does not need 416 KB of RAM to write |
  | `fe18d2f` | let the front-end own the pad and the mixer rate |
  | `e07a43f` | an XIP cart has no `gamepak_buffers`, and `load_gamepak` read the cart through them |
  | `fb0c4d7` | the save-type scan read a megabyte five times, and never let the watchdog breathe |
  | `8c03cf1` | let a front-end ask whether the cart's clock is actually running |
  | `0ca3f59` | let a front-end run M4A's software mixer natively |
  | `e321e63` | report the rate the game is clocking its DS FIFOs at |
  | `7887587` | the PSG channels were tuned to a sample rate we do not use |
  | `9d04646` | every PSG note-on was 5.39 semitones flat at 48 kHz |
  | `9cab230` | a conditional idle skip for raster polls |
  | `01f5e72` | Korean fan translations lose every override they were entitled to |

### The rebase is not mechanical — read this before doing it

libretro/gpsp master is now `5b6e751`, **8 commits ahead** of our base, and six of them are
sound work that overlaps ours:

```
3629cbe sound: mix at full 16-bit scale instead of 12-bit + x16 output shift
59485d1 sound: round PSG volume scaling to nearest instead of floor
20bee6f sound: round direct sound interpolation to nearest instead of floor
636a795 sound: parameterize output rate, derive all frequency steps from one source
1a213a0 sound: parameterize output rate, derive all frequency steps from one source
ffce654 libretro: add Sound Output Rate core option (65536 / 32768 Hz)
f40d23d Buildfix
5b6e751 Delete apply-sound-rate-option.sh
```

Upstream solved, its own way, the same problem `7887587` and `9d04646` solve. A rebase will
conflict there and the right resolution is almost certainly **keep upstream's
parameterization and drop ours**, not merge both — then re-verify with
`tools/gba_m4a/prove.sh` and by ear, because `3629cbe` changes the mix scale that the G&W
output path (`gba_pcm_submit()` → mono fold → low-pass → SAI) was tuned against.

Nothing else in those 8 touches the port's surface; the other 11 commits should replay
cleanly.

---

## 4. Build and verify

```sh
git submodule update --init --recursive

make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
     ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

The core reaches the SD card as two files: `/cores/gba.bin` (`.overlay_gba` +
`.overlay_gba_itc`) and `/roms/homebrew/gba.xip` (`.xip_gba` + `.rodata_gba` — the sprite
renderer, its rodata and the BIOS image, contiguous on purpose so it is **one** flash cache
entry). Both extraction rules are in `Makefile.common`.

Then, in order:

1. `bash tests/test_gba_xip_contract.sh` — the split still holds.
2. `bash tests/test_gba_m4a_wired.sh` — the HLE is really linked.
3. On hardware: launch a cart, listen, and read the pause menu's **Settings** page for
   where the frame actually goes.

---

## 5. Referenced but not shipped here

The playbook mentions these; they are development-side and live in `jshsakura`'s tree:

- **branch `feat/gba-probe`** — a guest-PC histogram over DWT, for finding where a frame
  goes on the device. Diagnostic build, never shipped.
- **`game-and-what`** — a separate repo holding the idle-loop finder and the measurement
  database. `gba_idle_loop.c` is generated there and copied here whole-file; do not
  hand-edit it. The hand-curated raster-poll entries are the exception and live in
  `main_gba.c` (`vcount_polls[]`).

---

## 6. The generated table

`Core/Src/porting/gba/gba_idle_loop.c` is **generated** — by `scripts/gen_gba_over.py` in
the `game-and-what` repo, from addresses measured by running the ROMs. Copy it whole-file;
do not hand-edit it, or the firmware quietly disagrees with the measurements it claims to
carry. Its own header says so.

The one hand-maintained exception is `vcount_polls[]` in `main_gba.c`: the conditional
raster-poll skips, which exist only for carts with no generated entry, because in gpSP the
two kinds of wait share one target slot.
