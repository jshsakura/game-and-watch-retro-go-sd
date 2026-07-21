# intflash vs upstream — feature inventory + merge-cost-aware recovery ranking

Compares against `upstream/main` = `sylverb/game-and-watch-retro-go-sd` (our direct
upstream, SD variant) and, where relevant, `upstream-nonsd/msx_wsv_genesis` =
`sylverb/game-and-watch-retro-go` (the non-SD original — different memory model,
ROMs compressed into flash rather than streamed from SD, so its own intflash
budget isn't a like-for-like number; used here only to check for techniques,
not byte comparisons).

**Constraint driving this doc**: we are 21 commits behind `upstream/main` and
1,074 ahead. A pending merge is real, ongoing work — any flash recovery that
reshapes a file upstream also edits makes that merge harder, every time. Every
candidate below is scored on that basis, not just recovered bytes. Where the
same bytes are recoverable at lower merge cost, prefer the lower-cost path.

All byte figures below are from `build/gw_retro_go.map` for the current build
(259,900 B intflash, both landed fixes applied — `d1dc577d`, `2f36536b`).
Anything not map-measured is marked **미검증(unverified)**, per the standing
rule after today's −6.02 KB-predicted-vs−2.2 KB-actual miss.

## What upstream's own pending 21 commits already touch here

Checked `git log HEAD..upstream/main --stat` for anything flash/linker/size
related, so we don't duplicate work upstream already did or step on a change
that's about to arrive:

- **`31a08000`** ("flash only: fix compilation when enabling cheats" etc.)
  touches `STM32H7B0VBTx_SDCARD.ld` (6 lines), `STM32H7B0VBTx_FLASH.ld`
  (23 lines), `Makefile.common` (4 lines), `rg_emulators.c` (8 lines) — **this
  is the exact file any of the pending overlay-capture candidates below would
  also touch.** Not a functional conflict (different content), but it confirms
  the linker script is an active file upstream is also editing right now.
- **`c1be726a`** ("Only store one non uk language in DTCRAM") — same spirit as
  our i18n footprint question, but it's about **DTCM** (RAM), not intflash.
  Different budget, not directly relevant, but shows upstream is doing
  parallel memory-budget work in the same area.
- **`8caa3e45`** ("Moved datas not needed during emulation into AHBM instead
  of DTCM") — also DTCM/AHB, not intflash.
- **`2116bedc`** ("Flash Cache: merged fixes from jshsakura's fork") — this is
  about the **external QSPI flash cache** (ROM/asset caching), unrelated to
  the 256 KB internal launcher bank this doc is about. Notable only because
  it's upstream absorbing *our* fork's earlier work — the merge relationship
  already runs both directions.
- **Nothing in the 21 commits touches** `lz4_depack.c`, `LzmaDec.c`,
  exception/unwind flags, or C64 (doesn't exist upstream) — the two already-
  landed fixes don't conflict with anything incoming.

## Feature inventory — what we've added that upstream doesn't have

| Item | Bytes (intflash) | Also in upstream-sd? | Confidence |
|---|---:|---|---|
| Clock app + alarm (`rg_clock.c`+`rg_clock_gif.c`+`rg_clock_album.c`+`rg_clock_alarm_mp3.c`+`rg_alarm.c`) | **21,403** (20,181 clock UI cluster + 1,222 `rg_alarm.o` — see note below on which part is actually overlay-movable) | No | map-confirmed |
| System grid home (`rg_system_grid.c`+`_layout.c`) | **1,116** | No | map-confirmed |
| Favorites (`rg_favorites.c`) | **1,079** | No | map-confirmed |
| Boot rescue (`gw_boot_rescue.c`) | **576** | No | map-confirmed |
| Update guard (`gw_update_guard.c`) | **446** | No | map-confirmed |
| Media/video/music player | not separately measurable | No | **Architecturally already correct — see below, not a recovery target** |
| Coverflow | not separately measurable without a `COVERFLOW=0` differential build | Unclear — need to check if upstream has its own coverflow flag | 미검증 |
| Extra emulator cores' resident footprint (GBA, NGP, 32X/picodrive, VB, WonderSwan, C64) | folded into `rg_logos.o`(11,060) / `rg_emulators.o`(9,781) growth below | No (none of these 6 are in upstream's `CORE_*` list) | see growth table |
| i18n framework + extra locale stubs | `rg_i18n.o` = 4,768 (framework + built-in `lang_en_us` table only — every other language's strings/fonts load from SD at runtime, not compiled in) | Partial — upstream has i18n too, just fewer locales | map-confirmed total, not confirmed delta |

Note on the clock total: `rg_alarm.c` is architecturally separate from the
clock UI cluster (it's the resident, always-on "is an alarm due" poller — see
`docs/CLOCK_APP_OVERLAY_SCOPING.md`), so listing it combined above overstates
what an overlay move could ever recover. The actually-movable portion is
**20,181 B** (`rg_clock.o` 18,083 + `rg_clock_gif.o` 884 + `rg_clock_album.o`
830 + `rg_clock_alarm_mp3.o` 384), minus the small resident `clock_ensure_dirs()`
stub the scoping doc carves out (not separately measured, well under 200 B).
`rg_alarm.o`'s 1,222 B stays resident regardless — it's the thing that lets
the launcher check "is anything due" without ever loading the overlay.

**Media/video/music player is not a recovery target — it's already done
right.** Confirmed via `Makefile.common:1921-1923`: both `Music.bin` and
`Video.bin` are extracted from `.overlay_music`, the same on-demand
RAM_EMU-loaded pattern every emulator core uses. Their bulk code costs zero
intflash already. This is worth stating explicitly since it's the pattern
`docs/CLOCK_APP_OVERLAY_SCOPING.md` proposes retrofitting onto the clock app
— **upstream doesn't have this feature to compare against, but our own media
player is the internal proof the pattern works for a launcher "app", not
just emulator cores.**

## Existing shared files, grown vs upstream (not cleanly separable into "our addition" bytes without upstream's own compiled size)

| File | Upstream lines | Our lines | Δ lines | Current total bytes (ours) |
|---|---:|---:|---:|---:|
| `rg_logos.c` | 2,527 | 2,958 | +431 | 11,060 |
| `rg_main.c` | 1,089 | 1,306 | +217 | 4,808 |
| `rg_emulators.c` | 1,496 | 1,688 | +192 | 9,781 |
| `rg_storage.c` | 530 | 604 | +74 | 1,372 |
| `rg_i18n_en_us.c` | 286 | 352 | +66 | ~0 (folds into `rg_i18n.o`) |
| `rg_rtc.c` | 256 | 319 | +63 | 1,180 |
| `rg_i18n.c` | 1,075 | 1,096 | +21 | 4,768 (combined w/ above) |
| `gw_firmware_abi.c` | 249 | 256 | +7 | 40 |
| `rg_frogfs.c`, `rg_welcome_prompt.c` | — | — | 0 | identical, not a factor |

`rg_logos.c`'s +431 lines and `rg_emulators.c`'s +192 lines are mostly the
extra cores (icon table entries + `emu_dispatch_t` rows for GBA/NGP/32X/VB/
WSWAN/C64) — real feature cost, not fat, and each core's overlay itself costs
zero intflash (only the launcher-side registration does).

## Recovery candidates, ranked by bytes × merge cost

| Candidate | Recoverable | Merge cost | Shared file(s) touched | Status |
|---|---:|---|---|---|
| **VB atof→strtod** | small (`atof.o` wrapper only — see libm doc, `strtod`/`mprec` chain stays resident regardless, it's needed by resident `gw_firmware_abi.o` too) | **Low** — VB doesn't exist in upstream at all | none upstream-shared (VB-only overlay block) | Tier-2, in progress |
| **MD32X sqrt** | part of libm Tier-2, not yet isolated | **Low** — MD32X doesn't exist in upstream at all | none upstream-shared | Tier-2, in progress |
| **TGBDUAL + LYNX unwind flags** (same fix as landed C64 one) | ~3.6 KB+ combined, **미검증** until built — today's estimate already missed once | **Medium** — touches `Makefile.common`'s tgbdual/lynx compile rules; both are upstream-shared cores (`d1191352` in the pending 21 touches tgbdual, but via submodule pointer + `linux/Makefile.gb-tgbdual`, not this exact STM32 rule — low direct-line conflict risk today, but the area is active) | `Makefile.common` | Found, not applied |
| **nes_fceu sin/log + amstrad exp** (libm Tier-2) | 미검증 | **Medium** — FCEUMM and caprice32/Amstrad are both upstream-shared cores | `STM32H7B0VBTx_SDCARD.ld` overlay-capture extension for each | Tier-2, in progress |
| **A2600 unwind-runtime overlay-capture** | ~3.6 KB (shares the same runtime TGBDUAL/LYNX would stop needing — actual number depends on doing those first) | **High** — touches the linker script, which is already the most-diverged file (700 lines vs upstream) and has a pending upstream commit (`31a08000`) landing in the same file | `STM32H7B0VBTx_SDCARD.ld` `.overlay_a2600` block | Found, not applied |
| **Clock app → `.overlay_clock`** | ~19.8 KB (largest single item, map-confirmed current cost) | **Medium-High** — content is orthogonal to upstream (clock doesn't exist there, so no *semantic* conflict), but it's a new block inserted into the same already-681-line-diverged, actively-churning linker script; mechanical merge friction (line-shift conflicts) even without content conflict | `STM32H7B0VBTx_SDCARD.ld` (new block), `rg_main.c` (3 call sites) | Scoped (`docs/CLOCK_APP_OVERLAY_SCOPING.md`), not implemented |

**Reading this table the way you asked**: at equal recovered bytes, prefer
the fork-exclusive-file candidates (VB, MD32X — nothing upstream can ever
conflict with) over the shared-file ones (TGBDUAL/LYNX/A2600/nes_fceu/
amstrad, all real upstream cores). Between the two shared-file categories,
prefer flag-only changes (TGBDUAL/LYNX, same proven shape as the already-
landed C64 fix, touches only `Makefile.common`) over linker-script surgery
(A2600, clock app) — the linker script is both the most already-diverged
file and the one upstream is actively mid-edit on right now.

## Recommendation given the pending 21-commit merge

Land the low/medium-merge-cost items (VB, MD32X, TGBDUAL, LYNX) as their own
small commits — same shape as the already-landed C64 fix, low risk, low
merge friction. Hold the two linker-script-touching items (A2600 capture,
clock overlay) until after the upstream merge lands, not before — doing them
first means re-resolving the same linker script conflict twice (once against
upstream's incoming `31a08000`-era changes, once for whatever else arrives
in the next batch). This mirrors the ordering already used for the C64 fix
itself: land the cheap flag-only win now, defer the file-restructuring win.
