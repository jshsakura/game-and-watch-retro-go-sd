# Changes vs upstream (sylverb/game-and-watch-retro-go-sd `main`)

Catalogue of what this fork (`jshsakura`, branch `feat/pcecd`) adds or changes
relative to upstream, to help plan reviewable upstream PRs. Figures are from
`git diff upstream/main..feat/pcecd`.

**Scale:** ~708 files, **+163,817 / −1,054** lines. The bulk of the insertions is
vendored/submodule emulator cores; the firmware-side changes are far smaller and
are what actually needs review.

> ⚠️ Not one PR. This is a multi-session, multi-system divergence. It should land
> as a **foundation PR** + **one PR per system**, each self-contained with a host
> harness. This document is the map, not a merge request.

---

## 1. New emulated systems (not in upstream)

Each is its own `Core/Src/porting/<sys>/` layer + a core (vendored under
`external/` or in the `retro-go-stm32` submodule) + a linker overlay + launcher
wiring + a `linux/` host harness.

| System | Core | porting LOC | Status |
|---|---|---|---|
| **C64** | Frodo (vendored `porting/c64/frodo`) | ~27,500 | plays, SID sound, save/resume, configurable keys |
| **Virtual Boy** | red-viper (`external/red-viper`) | ~610 | plays; interpreter ~70% speed w/ scoped auto-OC; gapless audio |
| **ZX Spectrum** | floooh/chips (`porting/zxs/chips`) | ~5,980 | plays, beeper+AY, configurable keys |
| **WonderSwan** | oswan (`external/oswan-go` fork) | ~2,540 | plays, scoped auto-OC |
| **Neo Geo Pocket** | RACE (`external/race` fork) | ~380 | plays |
| **Tiger game.com** | (vendored) | ~3,090 | plays |
| Atari Lynx | Handy (`external/handy-go` fork) | ~340 | plays, 512K carts, save/load |
| Music player | homebrew app | — | MP3 |
| Video player | homebrew app | — | MJPEG-AVI |
| DOOM / Wolf3D / nh-doom | — | — | **SHELVED** (RAM/XIP limits) — exclude from upstream |

*(Lynx / NGP / WonderSwan may already be in-flight to upstream via earlier PRs —
confirm before re-submitting.)*

## 2. Enhancements to systems upstream already has

- **PC Engine → PC Engine CD.** New files in `porting/pce/`:
  `pce_scsi.[ch]` (SCSI target), `pce_cd.[ch]` (CUE/BIN + streaming),
  `pce_adpcm.[ch]` (ADPCM). ~2,200 LOC. Plus core-side changes in the
  `retro-go-stm32` submodule (see §4): route `$1800` CD-ROM²
  I/O to the SCSI target, Super System Card signature at `$18C0-$18C7`, and
  **CD backup RAM (BRAM)** at bank `$F7`. Adds: boot/data, CD-DA (Red Book BGM),
  ADPCM voice, BRAM saves (`/data/pcecd.bram`), resume-safe audio.
- **Videopac → Odyssey²** device-load fixes (`porting/videopac`, o2em-go fork).

## 3. Shared firmware infrastructure (the review-sensitive bits)

Small, cross-cutting — these are the changes a maintainer most needs to see:

| File | ± | What |
|---|---|---|
| `Core/Src/porting/common.c` / `.h` | +14 / +1 | `common_emu_auto_oc(level)` — per-system, non-persisted CPU boost (OSPI1-guarded) |
| `Core/Src/retro-go/rg_main.c` | +131 | menu/OC plumbing |
| `Core/Src/retro-go/gui.c` | +171 | carousel empty-box fixes (H and V), theme visible-count |
| `Core/Src/retro-go/rg_emulators.c` | +343 | launcher list handling, duplicate-entry cleanup |
| `Core/Src/retro-go/rg_logos.c` | +1,381 | per-system icons + name wordmarks |
| `Core/Src/retro-go/i18n/*` | — | strings for the new systems, all 12 languages |
| `STM32H7B0VBTx_SDCARD.ld` / `_FLASH.ld` | +314 / +263 | one RAM/flash overlay per new core |
| `Makefile` / `Makefile.common` | +425 / +192 | per-core build rules, overlay packaging |

## 4. Submodule / vendoring changes (a structural hurdle for upstream)

- **`retro-go-stm32`** is pinned to a **jshsakura fork**, 4 commits ahead of the
  upstream pin — and these carry the **PCE-CD core** work (SCSI routing, SS card
  signature, BRAM) and the emulator cores. Upstream can't take the parent PRs
  until this is resolved: either merge those commits into upstream's
  `retro-go-stm32` first (then bump the pin), or vendor them.
- Other forks pinned: `o2em-go`, `FatFs`, `oswan-go` (jshsakura forks).
- New submodules added: `external/race`, `external/handy-go`, `external/red-viper`,
  `external/doomgeneric`, `external/wolf3d-stm32` (last two shelved).

## 5. Host test harnesses (validation evidence)

Under `linux/`, each mirrors the device build (same sources, same
`-DGNW_VB_DEVICE`-style ifdefs, same renderer) for deterministic, log/CRC-based
validation before flashing: `Makefile.{c64,vb,zx,lynx,gamecom,pce,...}` + dirs.
E.g. `linux/vb` A/B-proves renderer changes pixel-identical; `linux/pce` proves
CD-DA decode and BRAM save/load. These are the artefacts to attach to each PR.

---

## Suggested PR order (dependency-first)

1. **Foundation** — `common_emu_auto_oc`, overlay/linker pattern, MAX_EMULATORS,
   diag conventions, launcher icon/wordmark plumbing.
2. **Per system**, cleanest first to build reviewer trust:
   C64 → PCE-CD → Virtual Boy → ZX → WonderSwan → NGP → game.com.
   Each PR = core + porting layer + overlay + launcher wiring + host harness +
   a short "how it was validated" note.
3. Exclude shelved DOOM/Wolf3D/nh-doom.

Resolve §4 (submodule) **before** the PCE-CD and any core-bearing PR.
