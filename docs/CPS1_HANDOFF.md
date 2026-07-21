# CPS-1 (Tenchi wo Kurau II / Warriors of Fate) — Handoff

> Worktree: `/home/ubuntu/app/jupyterLab/notebooks/gnw-cps1`, branch
> `explore/cps1-feasibility`, isolated from the main session's working tree
> (`perf/32x-histogram` on the primary checkout) per this project's
> per-initiative-worktree convention. Nothing here has touched the main
> checkout or any real device system list — this is a feasibility/skeleton
> port, not a shipped core. Read this before continuing the work; it
> supersedes nothing in `CPS1_FEASIBILITY.md` / `CPS1_SENIOR_TRICKS_ANALYSIS.md`
> / `CPS1_MAME_ALIGNMENT.md` / `CPS1_ULTIMATE_PORTING_PLAN.md`, it indexes them
> plus everything built on top since.

## 1. What exists right now

A host-buildable, QEMU-M7-profileable CPS-1 skeleton:
- 68000 interpreter **and** goto-threaded static recompiler, diff-verified
  against each other frame-by-frame (see §4 for the one open bug in this).
- Real memory map: WRAM, 192KB gfxram pool with indirect CPS-A base
  registers (OBJ/SCROLL1/2/3/OTHER/PALETTE), one-frame-delayed OBJ RAM
  latch, CPS-B priority-bitmask registers (4 masks).
- PPU: 256KB direct-mapped LRU tile cache (packed 4bpp, not decoded), OAM
  prescan, sprite renderer (16x16 base unit, multi-tile blocks, X/Y flip),
  3-layer BG renderer (SCROLL1/2/3, palette offset +0x20/+0x40/+0x60,
  bit-swizzled tilemap addressing), priority-bitmask compositor.
- Sound: Z80/YM2151/OKI6295 HLE stand-in (**not real QSound** — see §5).
- Real ROM loading: gfx bitplane decoder, bank mapper for wof's ROM
  layout, reset-vector sanity check.
- Device-only integration code (compiles clean against real STM32H7xx HAL
  headers, **never flashed to real hardware** — see §4 for exactly which
  parts are unverified beyond that):
  - `Core/Src/porting/cps1/cps1_device_display.c` — LTDC dual-layer
    (layer 0 = SCROLL1+SCROLL2 combined "bottom", layer 1 = SCROLL3+sprite
    "top", hardware color-keying on layer 1 for transparency), plus
    center-crop from CPS-1's native 384x224 to the panel's 320x224.
  - `Core/Src/porting/cps1/cps1_device_audio.c` — mixer submission mirroring
    the shared `audio_start_playing`/`audio_get_active_buffer` pattern.
  - `Core/Src/porting/cps1/cps1_device_dma2d.c` — DMA2D_R2M framebuffer
    clear, and DMA2D_M2M_BLEND indexed-tile-to-RGB565 hardware blit via a
    loaded CLUT (color lookup table).
  - Real ITCM/DTCM linker-script wiring in `STM32H7B0VBTx_SDCARD.ld`
    (currently 0 bytes in every real build — see §5, "not yet linked in").
- `tools/m7_qemu_rig/rig_cps1.c` — QEMU Cortex-M7 instruction-count rig,
  same shape as `rig_vb.c` (see `docs/HARNESSES.md`).

## 2. Phase-by-phase commit table

| Phase | Commit | Summary |
|---|---|---|
| 1+2 | `2b3ec3b2` | PC/SDL2 host harness skeleton + M7 QEMU rig |
| — | `95f3dec6` | docs: Phase 1+2 status + harness usage |
| — | `8224a47e` | rename `main.c` → `main_cps1.c` |
| 3 | `887902c5` | minimal 68000 interpreter + static-recompiler translator skeleton |
| 4 | `1558c0c5` | ROM loader + PPU 256KB LRU tile cache/OAM prescan skeletons |
| 5 | `84e48f53` | wire OAM→tile-cache→palette into a real blit |
| 6 | `31800e7c` | parameterized GFX bitplane decoder + 68k↔VDP memory bus |
| cheat 8 | `1d2d92ff` | SCROLL1/2/3 layer renderer + compositor |
| cheat A | `019e5965` | Z80/YM2151/OKI6295 sound HLE player skeleton |
| 7 | `350268e8` | integrate CPU/BG/sound into the frame loop + fps profiling |
| — | `e00c9615` | docs: confirm GFX/video/sound spec against MAME, correct sound target |
| — | `15f9e32b`, `c37ef869` | docs corrections (plane-to-bit direction, contradictory bullet) |
| 8 | `ab18bc44` | GFX layout + palette correctness pass (MAME-confirmed). **Last commit where `--engine=diff` is clean — see §4.** |
| 9 | `945578d2` | real memory map + indirect gfxram addressing + one-frame OBJ delay. **Introduces the §4 diff-harness bug.** |
| 10 | `7fabef7b` | sprite/BG field-layout + priority (OAM attr word, BG cell layout, priority compositor) |
| 11 | `f3703826` | real ROM loading (bank mapper, reset-vector check) |
| 12 | `d0eaa69d` | real hardware integration — LTDC dual-layer, DMA2D-adjacent audio mixer. Produced autonomously by a background fork; independently audited before being trusted (see §6). |
| opt-1 | `52550719` | ITCM/DTCM section attributes, blitter fast-path+unroll, mixer unroll. Found+fixed a real meta-pointer aliasing bug mid-phase (see §6). |
| opt-2 | `3115349c` | DMA2D CLUT hardware blit, real ITCM/DTCM linker wiring (validated via scratch full-firmware link, not just compile), recompiler-vs-interpreter measurement. Opaque-tile blit was implemented then fully reverted per user feedback — not in this commit. |

`git log --oneline main..explore/cps1-feasibility` is authoritative if this
table drifts.

## 3. Current performance verdict (QEMU M7, `mps2-an500`, 340MHz assumption)

Re-measured fresh at HEAD (`3115349c`) while writing this handoff:

```
full (host-compositor stand-in)        ≈ 8,515,943 insn/frame
device-cost (SCROLL3+sprite only, historical) ≈ 4,357,735 insn/frame
ltdc (REAL total: device-cost + SCROLL1+SCROLL2) ≈ 6,322,671 insn/frame ≈ 18.60 ms @ 340MHz
ltdc-rc (same workload, recompiler engine)       ≈ 6,322,652 insn/frame ≈ 18.60 ms — 0.0% dispatch saving
budget (60fps @ 340MHz)                          = 5,666,666 insn = 16.6667 ms
```

**Verdict: OVER budget by ~11.6%** (18.60ms vs 16.67ms). Both optimization
phases reduced `full` by 25.8% relative to a hypothetical unoptimized
baseline, but the number that matters — `ltdc`, the real per-frame cost
under the actual dual-layer LTDC architecture — is still over.

**The recompiler is not the bottleneck (0.0% saving).** This was checked
explicitly at opt-2 (item 1 of the "core 3 areas" ask) precisely because it
was tempting to assume dispatch overhead was significant: it isn't, for
this synthetic test program, because the recompiler already translates
100% of that program's opcodes (interpreter and recompiler agree
bit-for-bit on checksum, by construction of the diff harness). There is no
uncovered-opcode fallback path in this harness to have expanded coverage
*of*. A real ROM's much larger opcode footprint would need actual
basic-block cache measurement this synthetic harness can't produce.

**Why ITCM/DTCM placement and DMA2D offload show up as exactly 0.0%
QEMU-visible change**, and why that is *expected*, not a failure of the
work: `mps2_an500.ld`'s own header states this rig models no wait-states or
cache effects — it counts real ARMv7-M Thumb-2 instructions on a real
instruction stream, but instruction count and instruction *cost* are the
same only when every access is equal-latency, which is exactly what
ITCM/DTCM placement and DMA2D hardware offload change on the real part
without changing instruction count at all (a DMA2D blit removes CPU
instructions entirely — reads as fewer instructions in a build that
actually calls it, not a QEMU-measurable "speedup" of the interpreter loop
itself). This is the project's own long-standing principle,
"명령어≠기기사이클" (instructions ≠ device cycles) — cited directly in
`CLAUDE.md`'s "Testing a core the way the device runs it" and demonstrated
repeatedly elsewhere in this repo (Virtual Boy's QEMU-icount blindness to
cache misses). **The only real judge of whether this port can hit 60fps is
the device's own DWT/frame ledger, on real hardware, which this initiative
has never run** (see §5).

## 4. Open bug found while writing this handoff — diff harness has had a frame-0 mismatch since Phase 9

Running the actual verification gate before writing this document (rather
than trusting prior phases' self-reported "OK") surfaced a real,
previously-unreported failure:

```
$ ./build/retro-go-cps1 --frames=600 --engine=diff
[cps1] MISMATCH frame 0: fb interpreter=ad425192 recompiler=21c7338e | cpu interpreter=e3c7c035 recompiler=e3c7c035
[cps1] FAIL: 1/600 frames mismatched
```

CPU-register checksums agree exactly; only the rendered framebuffer
diverges, and only on frame 0.

**Bisected precisely** (scratch `git worktree add --detach` at each phase
commit, never touching the real checkouts): `ab18bc44` (Phase 8) is clean —
`600 frames bit-identical`. `945578d2` (Phase 9) is the first commit where
this fails, and it fails identically at every commit from Phase 9 through
current HEAD (`3115349c`) — this has been silently broken through Phases
10, 11, 12, and both optimization phases.

**Root cause, read directly out of `cps1_core.c`, not guessed:** Phase 9's
one-frame-delayed OBJ RAM feature added `s_buffered_obj` — a *single*
global static, latched once per `cps1_core_run_frame()` call — but
`main_cps1.c`'s diff loop (`linux/cps1/main_cps1.c:81-82`) calls
`cps1_core_run_frame(CPS1_ENGINE_INTERPRETER)` then
`cps1_core_run_frame(CPS1_ENGINE_RECOMPILER)` **for the same frame index**,
sharing that one `s_buffered_obj` (and, structurally, `s_tile_cache`,
`s_synthetic_palette`, `s_bg`) between both engines instead of giving each
engine its own copy the way `s_engine[CPS1_ENGINE_COUNT]` already does for
CPU state. On frame 0: the interpreter runs first, renders with
`s_buffered_obj` still zeroed (correct — nothing has been latched yet),
then its own tail latches `s_buffered_obj` from the (engine-independent,
static) synthetic OAM data. The recompiler then runs for the *same* frame
0 and sees that already-latched OBJ data — one frame early — producing a
different sprite layer and thus a different composited checksum. From
frame 1 onward the shared buffer holds identical content regardless of
which engine's tail wrote it last (the synthetic OAM data never changes),
so the two engines' checksums converge and stay identical for the
remaining 599 frames — exactly matching the observed "1/600."

This is **not** a real 68000/recompiler correctness bug and **not** a
CPS-1-hardware-relevant bug — it's an artifact of the diff harness's
shared, engine-scoped-in-name-only render state failing to isolate the two
engines from each other for exactly one kind of state (frame-latched OBJ
RAM) that Phase 9 was the first phase to introduce. It does mean every
"bit-identical" / "OK" claim for Phases 9 through opt-2 in this initiative
and its prior session summaries was **not actually re-verified by running
`--engine=diff` at those phases** — the selftests (which don't run both
engines against shared state) stayed green throughout, which is why this
went unnoticed for four phases.

**Not fixed here** — this handoff is a documentation/organization task, not
a fix. The fix is straightforward once picked up: give `s_buffered_obj`
(and audit `s_tile_cache`/`s_synthetic_palette`/`s_bg` for the same
sharing, though those are read-mostly and engine-order-independent so far)
per-engine storage the same way `s_engine[]` already does, or restructure
the diff harness to run each engine's full 600-frame sequence to
completion before comparing checksums instead of interleaving frame-by-
frame. **Do this before trusting any future phase's diff-harness "OK."**

## 5. Honest limitations / what is NOT verified

- **Never run on real hardware.** No LTDC dual-layer precedent existed in
  this codebase before Phase 12 (`cps1_device_display.c`'s own header says
  so) — layer 1 configuration, color-keying, and the crop-to-panel math are
  compile-clean-against-real-HAL-headers verified only, nothing more.
- **DMA2D nibble order is an assumption, not a verified fact**
  (`cps1_device_dma2d.c`'s header note): the L4 input format's
  low-nibble-vs-high-nibble pixel-0 convention is taken from ST's
  documented convention to the best of this project's current knowledge,
  opposite of `cps1_rom_decode_tile_planar`'s own output, hence the
  explicit nibble-swap. If backwards, the real-hardware symptom is every
  adjacent pixel pair swapped — a recognizable, specific artifact, not a
  crash.
- **ITCM/DTCM sections are 0 bytes in every real build right now.** cps1
  has no system-list entry, no APPID, no call graph any real linked
  firmware reaches — `--gc-sections` correctly strips it all. The linker
  script sections exist and were validated via a real scratch full-
  firmware link (not committed — reverted via `git checkout --` on the
  main checkout every time), confirmed byte-exact placement inside
  ITCMRAM/DTCMRAM when force-`KEEP()`'d for that one diagnostic test. They
  are deliberately **not** `KEEP()`'d in the real committed section — once
  cps1 is genuinely wired into a system list, gc-sections will retain the
  reachable code naturally, the same way it already does for every other
  core (PCE's own `.overlay_pce_itc` is the precedent this mirrors).
- **Sound is not QSound.** Tenchi wo Kurau II / Warriors of Fate's real
  arcade sound hardware is Capcom's custom QSound DSP
  (`docs/CPS1_MAME_ALIGNMENT.md` §0's "critical correction"). The current
  HLE is a Z80/YM2151/OKI6295 stand-in — a real QSound implementation
  (or QSound-HLE, mirroring this project's SNES N-SPC HLE precedent) is
  still an entirely separate, unstarted body of work.
- **Renderer uses an 8x8-sub-tile-decomposition convention**, not
  `CPS1_GFX_LAYOUT_16X16`/`_32X32` directly — functionally equivalent for
  the tiles this skeleton has exercised, but not the real hardware's raw
  layout; rewiring to the real layout is deferred, unstarted work.
- **Pre-existing, unrelated main-branch flash-budget crisis** was observed
  while scratch-testing the linker script against the main checkout (A/B
  stash test: baseline overflows FLASH by 11,860 bytes, this change's own
  4-byte section-header padding brought it to 11,864) — confirmed
  pre-existing and NOT cps1's fault; not this initiative's job to fix, but
  worth knowing before anyone tries a real full release build with cps1
  linked in.
- **§4's diff-harness bug** — see above; treat every pre-this-handoff
  Phase 9+ "bit-identical" claim in this initiative's history as
  unverified until the shared-state fix lands and `--engine=diff` is
  re-run clean.

## 6. Process notes worth carrying forward

- **Phase 12 was produced autonomously by a background fork** dispatched
  for narrowly-scoped research, which drifted into full implementation and
  a real commit (`d0eaa69d`) without being asked to. It was independently
  audited (not blindly trusted) before being built on further — the audit
  found the LTDC-precedent asymmetry documented in §5 but no functional
  defect. Treat any fork/subagent output the same way: verify by
  execution, especially when its own self-report claims verification.
- **A real, non-cosmetic bug was found and fixed mid-optimization**: the
  first version of the unrolled fast-path tile blit
  (`cps1_blit8x8_row_fast`) shared a single `uint8_t *m = meta_row;`
  pointer across all 8 pixel writes instead of `&meta_row[N]` per pixel —
  silently broke the priority compositor's layer-picking while leaving
  raw per-layer color checks passing. Found via a sequence of
  increasingly-isolated scratch reproductions, not by inspection alone.
  Fixed and verified bit-identical to the pre-optimization baseline
  afterward (this fix predates and is unrelated to §4's bug).
- **User feedback reversed a planned optimization mid-session**:
  opaque-tile (no-transparent-pixel) fast-path blitting was implemented,
  then explicitly reverted per the user's own domain reasoning — CPS-1
  uses pen-index-0-transparent uniformly for both sprites and BG, so a
  per-tile runtime opacity check trades a real branch-misprediction/
  compute cost for a benefit that doesn't materialize. Fully reverted via
  `git checkout --`, not included in any commit. Don't re-propose this
  without new information.
- **The riskiest change this initiative made — editing the real,
  production `STM32H7B0VBTx_SDCARD.ld`** — was validated the hard way: not
  just "does it compile" but an actual scratch full-firmware link attempt
  against the main checkout, producing and inspecting a real `.map` file,
  every time reverted via `git checkout --` and never left modified on
  that separate working branch. This is the same discipline
  `CLAUDE.md`'s "Testing a core the way the device runs it" describes for
  cores generally — it applies just as much to linker-script edits made
  from an unrelated worktree.

## 7. Suggested next steps (not started, in rough priority order)

1. Fix §4's diff-harness shared-state bug and re-verify `--engine=diff`
   clean before trusting anything further.
2. Close the ~11.6% frame-budget gap — likely candidates not yet tried:
   basic-block cache coverage against a *real* ROM's opcode footprint
   (this synthetic harness can't measure that), and whatever the real
   device DWT ledger says once this is ever flashed (§5 — currently never
   done).
3. Real QSound (or QSound-HLE) implementation to replace the current
   Z80/YM2151/OKI6295 stand-in.
4. Wire cps1 into a real system list / APPID *only* after 1-2, given
   APPID additions reset every user's persistent settings
   (`CLAUDE.md`'s "Adding an APPID resets every user's settings").
