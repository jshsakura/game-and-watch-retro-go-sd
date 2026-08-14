# Optimization ledger — what was tried, what closed, and why

Read this **before** proposing a performance change. Every entry below cost real
time to establish, and several were re-proposed and re-abandoned more than once
because the result lived only in a session log. This file is the single place a
closed path is recorded.

Entries are organised by system, because that is how the work is organised. A
line here is not a suggestion — it is a road that was driven to the end.

## How to use this

1. Find your system. Read its **Closed** list first.
2. If your idea is there, it is closed. Reopening requires naming **what changed**
   — a new measurement, a new hardware fact, a new toolchain behaviour. Re-deriving
   the same reasoning is not a reason.
3. When you close a path, add it here the same day, with the number that closed it.
4. **Record what was reverted too.** Something that was committed and then backed
   out is not "never tried"; it is "tried and rejected", and the next person needs
   to know which.

---

## When does a large win exist at all?

Worth stating up front, because it predicts which systems will yield. A dramatic
lever exists only when the guest is doing one of two things:

**(a) Useless work** — remove it, for free, because the result does not change.
WonderSwan idle-skip cut emulation by 77%. SNES spin-skip already takes 56.6% of
SMW's instructions.

**(b) Software-emulating a standard component the host does natively** — replace it
wholesale. The GBA's M4A mixer was 27–60% of the frame. The SNES APU (SPC700 + DSP)
was 26%, and removing it took SMW from 46 to 55.4 fps.

If neither applies there is no clever bypass, and what remains is proportional work
— several 2–3% wins stacked, not one large one. In that regime the target also
changes character: the device is memory-bound, so what has to shrink is bytes moved
and cache misses, not instruction count.

Check which case you are in before planning the work. Getting this wrong in either
direction is expensive — assuming a lever exists wastes weeks, and assuming none
exists leaves 36% of a frame on the table (see the SNES colour-math entry below).

---

## Measurement discipline

These are the rules that stopped bad numbers from becoming shipped decisions.

- **Instruction count is not device cycles.** A QEMU icount rig cannot see cache
  misses or execute-in-place stalls. Virtual Boy's blit was predicted at 6.86 ms
  and took 23 ms on hardware — roughly 70% of it in stalls the rig is blind to.
  Use the rig for correctness (bit-identical framebuffer and audio hashes) and for
  relative comparison; judge absolute framerate and any memory-locality change on
  the device.
- **A probe that does not remove work has not measured the work.** Stubbing a
  function often moves its cost elsewhere while the total stays flat. If the total
  did not drop, the number is fiction. A difference of exactly zero is evidence the
  code does not run at all.
- **A harness must compile the file it claims to test**, and must link what the
  device links. A harness that is a different program proves nothing about the
  firmware.
- **Verify a gate actually arms.** A feature can be fully implemented, fully
  linked, and permanently off. See the SNES audio-HLE entry.
- **Do not judge architecture from the armchair.** On 32X the first estimate
  ("arithmetically impossible") and the second ("paging makes it comfortable")
  were both wrong; the truth was in between, at zero margin.
- **Two samples that do not overlap are not a result** until you have shown that
  the *scene* repeats. Super Mario World read 30.62–38.85 drawn fps from ONE
  unchanged binary — a 23% spread — and the first two samples of each arm landed
  so that a −9.2% regression looked real. Seven samples per arm, in one session,
  killed it. Nothing had regressed.
- **Repeatability is a property of the scene, not of the tool.** `drawn_ab.sh`
  repeats to ~1% where the overload guard is **pinned** — draw ratio sitting
  exactly on the floor (0.2500) or the ceiling (1.0000), where the forced-draw
  constant decides every frame. Where the pacing integrator is deciding
  frame-by-frame the ratio wanders (SMW 0.50–0.63) and so does the number. Read
  the ratio column first: **an exact ratio means the arithmetic is deterministic
  and two samples suffice; a wandering one means take seven.** Every 32X number
  in this file's 32X section is from a pinned scene (0.2500 or 1.0000).

---

## SNES

Deepest-worked core in the tree, and the one most likely to be re-proposed to.
Detail lives in [SNES_NEXT_SESSION.md](SNES_NEXT_SESSION.md) (state and open
threads), [SNES_WAIT_LOOP_BAKE.md](SNES_WAIT_LOOP_BAKE.md) and
[SNES_ROM_SURVEY.md](SNES_ROM_SURVEY.md) (the last shipped lever and how it was
verified across a library).

**Before proposing anything here, read these two instruments.** Most rejected
SNES proposals were not wrong ideas; they were right ideas judged with the wrong
number.

1. **fps is not what the player sees.** The overload guard draws roughly one
   frame in four, so a change that makes *skipped* frames cheaper LOWERS the fps
   counter — the guard spends the slack on drawing. `SNES_SPRITE_SKIP_DRAW` was
   shelved as "nothing, 52.29 vs 52.36" for exactly this reason. Measure
   `g_snes_drawn_frames` (`tools/gnw_probe/drawn_ab.sh`) for anything on the
   frameskip path.
2. **The attract screen is a different machine from real play.** It is a still
   image running fewer opcodes: it understates every per-opcode tax and
   overstates every cache. `SNES_LINE_CACHE` shipped in the wrong position
   because of this (−3.24 fps once measured in gameplay). Resume a savestate;
   `GNW_AUTOSAVE_FRAME=n` makes the console write its own scene.

**Shipped.** N-SPC timer-wait charging (`SNES_SPC_IDLE_SKIP`), the PPU
virgin-z test, the blend LUT, sprite-eval skip on skipped frames, the opaque
tile path, bulk general-DMA transfer, gap-free audio, the **baked wait loop**
(`SNES_SPIN_BAKE`, +35–97% drawn frames, 413/413 cartridges hash-identical), and
the **fetch-page cache for the carts that never had one** — `snes_cpuRead`'s
page cache was gated on `cart->romMask`, which is only set for a power-of-two
ROM, so every 3 MB cart took the full `snes_read → cart_read → cart_readLorom`
path on each opcode fetch (Super Metroid: 30,463 of 34,314 bus reads a frame),
and `cart_attachDsp1()` separately cleared `romMask` across the whole bus for
LoROM DSP boards. A per-bank base table built once at load fixes both: **Super
Metroid 47.5 → 60.9 emulated (+28%), 11.9 → 22.4 drawn (+88%); Pilotwings 44.9 →
59.1 (+32%)**, 14 cartridges bit-identical on the rig (`05fea2df`, arms
`SNES_ROMPAGE_FOLD=0` / `SNES_DSP_FASTPATH=0`).

**Closed — the whole-core approaches.**

- **Static recompilation (rc), every variant.** XIP: 46 fps → 3.5 on device
  (instruction count fell 42%; I-cache stalls ate all of it). ITCM: 44 fps
  against spin-skip's 46 — ⚠️ confounded, rc *replaces* spin-skip and the
  combination was never measured. Re-evaluated from scratch in 2026-08
  ([RC_DISPATCH_ANALYSIS.md](RC_DISPATCH_ANALYSIS.md),
  [RC_PRIOR_ART.md](RC_PRIOR_ART.md),
  [RC_FEASIBILITY_2026.md](RC_FEASIBILITY_2026.md)) and closed again on three
  independent grounds: it is **per-ROM** (8,371 site functions for SMW alone, so
  it does nothing for a generic core), the dispatch lookup costs 11–12
  comparisons with heavy branch misprediction on Cortex-M7, and the sites are
  high-frequency indirect branches into QSPI flash — the same wall that killed
  DOOM's XIP.
- **The spin-skip learner, at any setting.** −4.78 fps on hardware. The tax is
  the discovery machinery (bus hooks, purity proof), not the decision: the
  per-ROM whitelist already answered "no" for the ROM being measured and the
  4.78 was still charged. What replaced it executes the two opcodes instead of
  proving anything — see the bake doc.
- **Extending ITCM** — device A/B 41.3 → 40.9 fps, a net loss. The I-cache was
  already doing that job.
- **N-SPC HLE** (`SNES_NSPC_HLE`) — breaks 38% of the games that use the driver.
  Must stay default OFF.
- **The APU as a whole** — 22–26% of the frame, and both engines *together*
  (65816 + APU frozen) are worth **+2.83 fps** against a +2.95 distance to the
  audio-DMA cap. **The sum of every lever that exists is short of 60 emulated
  fps.** That is the arithmetic that closes the "find one more lever" framing.

**Closed — the renderer.** The ceiling is measured: deleting the *entire*
remaining render is worth **+3.15 fps** and no more. The share is large; the
headroom is not.

- pixel loop as SIMD pairs (−7.5), coarse 4-pixel transparent skip (+0.22,
  noise), sprite two-pass reverse draw (neutral), software pipelining at depth
  1/2/batched (all nothing), `PpuWindows_Calc` duplicate elimination (0 of
  42,191 sub passes duplicate), colour-math 2 px/iteration (−4.8%).
- **Every memory theory, four in a row at zero:** tile memo (80% hit rate),
  prefetch, framebuffer cache-pollution removal, and putting the whole of VRAM
  in DTCM. The rig then explained why: of the layer draw's 685,758 instructions
  a frame, **663,322 are the pixel work** — 97% of the instructions and 100% of
  the time. There is no stall mystery. The fetch, the tilemap walk and the
  per-call setup are all free.

**Closed — the DSP and audio output.**

- DSP idle-voice BRR skip (−3.1%), idle-skip delete + channel pointer hoist
  (−0.64; gcc had already folded the hoist), and both closed-form ways of
  dropping idle voices from the loop (0.00 and −0.59). 68% of voice ticks are
  idle voices — **68% is a count, not a cost.**
- **32 kHz output** — looked free (the DSP already synthesizes 534 samples a
  frame and the box filter throws half away). On hardware it costs **62% of the
  drawn frames** (52.65 → 21.2), and scaling the stretcher constants with the
  rate does not recover it. Closed.
- Audio stretcher: reversed noise fillers (219 underruns), loosening the
  insertion guard (0 → 444 underruns), per-passage noise smoothing (739). The
  ±1% band is a stability limit, not only an audibility one.

**Closed — the frame loop and pacing.**

- **Do not add a test to `run_dots`' inner loop.** A build that installed
  *nothing at all* still lost 6.6% of ALttP's drawn frames and 10.5% of Mario
  Kart's. Specialising the frame loop into two clones so the disarmed one folds
  the test away is worse: instantiating both changed gcc's register allocation
  and the never-installing build lost **13.9%**. Ask once per span, not once per
  opcode.
- Advancing the pacing reference by one period (−0.30): a 21 ms frame advances
  the DMA tick counter by 1, exactly like a 14 ms one. **You cannot bank slack
  in that counter.**
- Whole-D-cache clean before present (−0.19). `SystemClock_Config(3)` — dead
  code, the PLL stays at level 2, so there is no clock headroom either.
- 4bpp tile decode cache — no-go. Fold LUT — no gain.
- **Adding SNES as a system was previously closed at 24 fps** with the PPU as the
  wall; the current generic core is a separate effort and is not that proposal.

**Research notes kept for the record**, so a rejected road can be re-read instead
of re-driven: [SNES_APU_RESEARCH.md](SNES_APU_RESEARCH.md) (where the APU's
23.6% actually sits, per function),
[SNES_STATIC_RECOMPILATION.md](SNES_STATIC_RECOMPILATION.md) (public precedents
for PC→native dispatch under a 100 KB budget),
[RC_ACTIVATION_VERIFY.md](RC_ACTIVATION_VERIFY.md) (the host gate that has to
pass before flashing an rc build) and [RESUME_GNW.md](RESUME_GNW.md) (the
original rc resume point — note its §3 framing is superseded by
`RC_FEASIBILITY_2026.md`).

**Closed — HDMA, 2026-08-14, and it is a lesson about counting calls.** Device,
Super Mario Kart's savestate play scene, `SNES_DEVICE_PROFILE=1` Ledger B:
`hdma` = 33,410 cycles = **0.3%** of an 8,631,610-cycle drawn frame. The rig
says why: over 6,000 frames Mario Kart activates **no HDMA channel at all** —
`dma_doHdma()` is called every scanline (`dohdma=225/frame`) and its eight-channel
loop finds nothing enabled, so `hdmaTimer` stays 0 and the CPU never stalls.
Kart's Mode 7 perspective table is written per line from an H-timer IRQ, not by
an HDMA channel. **The 226 calls a frame were real and the work behind them was
not.** Where HDMA does run (Suzuka 8 Hours: 1,374 two-dot stall steps a frame =
0.77% of the dot clock) folding the stall is bit-identical and costs instructions
— 4,536,365 → 4,546,225 per frame — and was reverted.

**Still unmeasured** — one thing: the native ports' own frames. Super Metroid
runs 56.2 fps on hardware with **no interpreter at all**, and nobody has profiled
what that frame is. It is game C plus the PPU/APU emulation every ported game
shares, so whatever is in there is shared.

**Two instrument defects found 2026-08-14 — check these before trusting an old
number.** `run_snes_t2.sh` did not compile `dsp1_hle.c`, so `dsp1_alloc()` hit a
weak stub and **every DSP cartridge ran on the rig with no coprocessor attached**;
past rig numbers for those carts are void. And a `SNES_DEVICE_PROFILE=1` build
cannot inline `run_one_opcode` (the DWT marks block it), so gcc emits an `.isra.0`
clone into `.text` which the linker sweeps into `.overlay_snes` — PC samples
charged ~14% of the frame to **a program the device does not run**, and Ledger B's
"event scheduler" residue was inflated by the same amount. Take PC profiles on a
shipping build.

**Trap.** The audio-HLE gate compared 21 header bytes against `"SUPER MARIO WORLD  "`.
The real internal title has no space (`SUPER MARIOWORLD`), and a 19-character literal
in a 21-byte array leaves NULs where the header pads with spaces. The compare never
matched — not even for the vanilla ROM — so native sound was silently off and the
build was one release away from regressing 55.4 fps back to 46. Gating now identifies
the *sound engine* by its ARAM driver signature rather than the cartridge.

**Related trap.** Never gate a feature on a whole-ROM hash. Translation patches,
hacks and revisions all change it, and every one of them loses the feature. Identify
the engine — driver signature, code-region hash, opcode pattern.

## Sega 32X

**Removed from the firmware, 2026-07-27** — core and picodrive submodule both.
Doom reached 13–16 fps and After Burner 8 after a full day of optimisation; 60 fps
needs a frame to fit in 5.2 M device cycles and it costs ~24 M, a factor of 4.6.
The whole ledger is [32X_CLOSED.md](32X_CLOSED.md) and
[32X_DEVICE_MEASUREMENT_LOG.md](32X_DEVICE_MEASUREMENT_LOG.md). `APPID_32X = 26`
is kept as a RETIRED slot: removing an APPID shifts the struct and resets every
user's settings.

The entries below are kept because they are *hardware* facts that outlive the
core — the next person to reach for DMA2D or ITCM on this part needs them. The
last open bug at removal time, the overlay border flicker, is diagnosed in
[MD32X_MENU_FLICKER_ANALYSIS.md](MD32X_MENU_FLICKER_ANALYSIS.md): picodrive
renders only rows 8..231 and there is no RAM for a third buffer, so the two LCD
buffers alternate between different contents. It is a two-buffer problem, and it
will recur in any core that repaints under `odroid_overlay_dialog`.

**Closed.**

- **SH-2 idle-skip** — measured **zero** device framerate effect (the rig showed
  18–75%) and it destroyed Doom's gunshot sound effect. Removed permanently. If it
  is ever revisited it must fingerprint by opcode pattern, never by whole-ROM CRC.
- **DMA2D for the compositor and tile renderer** — not possible on this part; there
  is no colour-key hardware. Established from the register documentation, not
  guessed. Only the `FinalizeLine555` CLUT pass is theoretically eligible, and that
  needs a pipeline that does not exist yet.
- **`lcd_clear_buffers()` in this path** — it has no swap-pending guard, so the
  write-buffered clear overtakes the scanout beam and produces a black band along the
  bottom. Use `lcd_clear_active_buffer()`.
- **The existing ARM assembly** — it is A32, not Thumb-2, so it cannot run on the M7.

**Trap.** `Draw2FB` is allocated unconditionally from AHB during `PicoInit()`, so
anything else placed in AHB overflows on every launch. The hardware budget harness
catches this; the device would have caught it later and more expensively.

## Sega CD

**Folded, 2026-07-24, on a memory fact and not on a bug.** An accurate core needs
PRG 512 K + Word 256 K = 768 K of *writable* RAM against `RAM_EMU`'s 724 K, and
XIP does not help RW data. gwenesis only fits by cutting PRG to 128 K, which is
what breaks the Sub-CPU handshake — so the boot failures were a symptom of the
budget, not a separate defect to chase. External PSRAM is the only route.

**And the screen has never come up on the device, not once.** Earlier notes read
as a regression; they were host-harness results mistaken for device results. The
CDC/DECI chain analysis that was in flight is kept in
[SEGACD_INVESTIGATION.md](SEGACD_INVESTIGATION.md) so the next attempt does not
re-derive it.

**Trap.** A "permanently parked semaphore" was diagnosed from a 60-frame snapshot.
Live register reads showed the semaphore cycling normally every frame. Sparse
observation makes repetition look like a stop.

## CPS-1

**Abandoned by the owner, 2026-07-25.** Kept here because most of it was proven
and someone will propose it again. The renderer is bit-exact on the host — GFX
interleave, scaler and logic all verified — and the sound data path (tables,
packer, loader) is complete; the Z80 + QSound engine was never wired. On the
device it draws a solid colour and never reaches a title screen (flash-cache
ring / 68000 divergence). Preserved on branch `feat/cps1-container`; write-up in
[CPS1_HANDOFF.md](CPS1_HANDOFF.md).

## GBA

Shipped. The M4A mixer HLE is the lever that matters (27–60% of guest time).

**Closed.**

- **A dynamic recompiler** — gpSP's backends are x86, ARM32, A64 and MIPS. There is
  no Thumb-2 backend in existence. Every argument about JIT cache budget is moot.
  This was refuted twice in one session.
- **Moving `read_memory` and friends to ITCM** — the linker rejects it: the overlay
  section is declared before the ITC section, so the text glob claims those objects
  first, and the order cannot be reversed without breaking the load address ordering.
- **Moving `memory_map_read` (32 KB) to DTCM** — only 5.8 KB of DTCM is free. Halving
  the heap to buy 8% is not a trade worth making.
- **Registering an idle loop for Tales of Phantasia** — the game already idles in a
  `SWI 5` halt, so idling is free. Measured; the assumption was exactly backwards.

## Virtual Boy

Shipped and playable. The bottleneck differs per game — Wario is blit-bound, 3D
Tetris is interpreter-bound (the floating-point hypothesis was tested and rejected).

**Closed.** Lazy flags plus threaded dispatch was estimated at 20–25% but touches
every game's core path for an uncertain return. A dynamic recompiler is ARM32 only.
The accepted position is that the 3D titles run at their limit.

## WonderSwan

Complete: full speed at 75 fps, 27 glitches eliminated, overclock removed.

The lesson worth carrying: **state-exact is not cycle-exact.** The first idle-skip
reproduced machine state perfectly and still shifted mid-frame raster by a scanline
in 27 games, because it froze the cycle carry and delivered interrupts at the top of
the loop. A skip has to reproduce the cycle flow the scheduler sees *and* where the
CPU is when the interrupt lands.

---

## Cross-system

- **Overclocking is the last resort, not a lever.** Level 0 is 280 MHz, level 1 is
  312, level 2 is 340 — 9% for the whole climb. The project's position is to route
  around the CPU wall with technique (idle-skip, interpreter work, sound HLE,
  renderer locality), not to push clock.
- **Every core is an overlay at the same RAM address.** A symbol only another core
  defines links silently to that core's address. Namespace globals via the core's
  redefines file so a missing definition is a link error.
- **Internal flash is effectively full** — 262,100 of 262,144 bytes at the time of
  writing. Any growth in launcher or shared code breaks the link.
- **Never write to the SD card inside an emulator's frame loop.** It corrupts the
  FAT and takes the whole device down. Collect diagnostics in RAM and flush once, at
  load or exit.
