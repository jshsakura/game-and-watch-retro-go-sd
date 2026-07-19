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

---

## SNES

**Open / current.** Colour math is the largest remaining lever: the colour-math
loop is 89% of PPU time, which is ~36% of the whole frame, and roughly a third of
that is a bypass with no visual effect — i.e. case (a) above, not a micro-optimisation.
Work happens in the `external/sm` submodule, so it must go through a managed patch,
never a direct edit.

**Closed.**

- **Static recompilation (rc) via XIP** — 46 fps to 3.5 on device. Instruction
  count fell 42%; the I-cache stalls ate all of it and more. Dead road.
- **rc in general** — rc-ITCM measured 44 fps against spin-skip's 46. ⚠️ That
  comparison is **confounded**: rc *replaces* spin-skip, so it compares rc-alone
  with spin-skip-alone, and the combination was never measured. The honest claim is
  that rc's instruction reduction failed to beat spin-skip, for reasons still
  unestablished. Do **not** cite this as proof that the device is memory-bound —
  the direct evidence for that is the Virtual Boy blit, above.
- **4bpp tile decode cache** — no-go.
- **Fold LUT** — rejected, no gain.
- **Adding SNES as a system was previously closed at 24 fps** with the PPU as the
  wall; the current generic core is a separate effort and is not that proposal.

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

**Open / current.** Border flicker after opening the in-game overlay: picodrive
renders only rows 8..231, so the remaining 16 rows keep whatever the overlay left,
and the two LCD buffers alternate between different contents. After that, the draw
path (26.7% of the frame).

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

**Open / current.** Word-RAM 1M/2M bank ownership is unimplemented, and that — not
the coroutine state index stalling at 8 — is the boot blocker. The stall is a symptom.

**Closed.** Full emulation remains structurally out of reach: an 840 KB resident RAM
floor and two 68000s with no dynamic recompiler.

**Trap.** A "permanently parked semaphore" was diagnosed from a 60-frame snapshot.
Live register reads showed the semaphore cycling normally every frame. Sparse
observation makes repetition look like a stop.

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
