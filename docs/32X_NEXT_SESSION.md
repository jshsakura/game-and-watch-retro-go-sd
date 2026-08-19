# Sega 32X — where this stands, and what to aim at next

Written 2026-08-15, at the end of a day that produced one large win, one large
correction, and one axis closed by measurement. Read `32X_CLOSED.md` §0 and §0b
first; this file is the working state and the queue, not the ledger.

Everything below is measured on hardware unless it says otherwise.

---

# 2026-08-20 — the SH-2 dispatch overhead, and what the rig can and cannot predict

**Device: 24.01 -> 25.46 drawn fps (+6.04%), 3 samples, spread 0.02.**
Baseline arm `baseline24` (picodrive `48d3f5b6`), new arm `disp1`
(picodrive `00a11484`), same session, card `cores/32x.bin` md5s verified
different (`70f8112c` vs the baseline's).

## What shipped

Four cuts to the SH-2 interpreter's per-dispatch bookkeeping. Direct-path
bookkeeping went from **22 host instructions per dispatched guest
instruction to 15**; none of them changes what the interpreter computes.

| # | change | rig | commit |
|---|---|---|---|
| 1 | `BUSY_LOOP_HACKS` compiled out for `GNW_32X_CORE` | -1.79% | picodrive `6889be8e` |
| 2 | fastloop nibble gate moved into the switch cases it duplicated | -3.38% | same |
| 3 | pc round trip and the redundant `delay = 0` store removed | -0.45% | same |
| 4 | opcode fetch window hoisted out of the per-instruction path | -1.20% | picodrive `00a11484` |
| | **compounded** | **-6.67%** | device **+6.04%** |

(1) is the one worth remembering. Both `BUSY_LOOP_HACKS` blocks in
`mame/sh2.c` compare the opcode at `sh2->ppc` against the one they expect to
*follow* them -- but `ppc` in this dispatch loop is the executing
instruction's own address, so neither comparison can ever be true.
sh2pico.c's own fastloop comment already said "inert here". Inert, and still
issuing a guest 16-bit read on every dispatched `DT` and every `BRA`.

(4)'s safety argument is a property of the code, not an assumption:
`gnw_fw_rom` is written only by `bank_switch_rom_sh2()`, whose callers are
`PicoMemSetup32x()` and `p32x_update_banks()`, and everything it derives from
is fixed at cart load. Neither can run inside `sh2_execute_interpreter`. A
future runtime writer breaks it silently -- the comment says so, and
`-DGNW_FW_NO_HOIST` is the escape.

## ★ The rig predicts one kind of lever and gets the sign wrong on the other

This is the most useful thing today produced, and it now has three data
points on each side.

**Instruction-removal levers in the SH-2 interpreter map correctly.**
`disp1`: rig -6.67% -> device +6.04%.

**Pixel/memory-path levers do not just miss the magnitude, they miss the
sign.** QEMU has no cache and no wait states, so it cannot price a wide
store against the per-pixel work it replaces:

| lever | rig said | device said |
|---|---|---|
| blank nametable row cache | -1.82% (a gain) | **-1.46% fps** (2026-08-19) |
| removing the compositor's solid-run detector | -0.48% (a gain) | **-2.95% fps** (arm `nrd1`, 24.71 vs 25.46) |

`nrd1` is the sharper of the two, because the run detector's whole job is to
replace per-pixel `u16` stores with `u32` blasts. On the rig those blasts are
just instructions and the detector's failed probes are pure tax, so removing
it "wins". On the device the blasts are the point.

**The rule that follows:** a 32X lever that removes instructions from the
SH-2 interpreter may be proposed on rig evidence. A lever that changes how
many bytes move, or in what width, may not -- it must be benched on the
device, and the rig's sign is not even a hint. Both directions are now in
`32X_CLOSED.md`'s ledger.

A corollary nobody has cashed in yet: **the rig has therefore been
under-pricing the compositor's wide-store paths all along.** The quad
composite path was measured at 0.003% on the rig and dismissed; on the
device it may be worth several percent. Re-pricing `-DGNW_PP_NO_QUAD` on
hardware is a one-flag arm and is the first thing to do if the compositor is
revisited.

## The 68K was spending three quarters of itself in one spin

New instrument, `RIG_M68K_HIST` (picodrive `2a28aed8`, exact-PC census for
the 68K). On Doom's gameplay anchor, **75.2% of every 68K instruction
executed** is in one two-instruction loop:

    0x8832a2  tst.b  $ff1134.l      ; a flag in the 68K's own work RAM
    0x8832a8  beq.b  $8832a2        ; wait for the VInt handler to set it

That is `SekIsIdleCode`'s 6-byte `tst.b ($xxxxxxxx)` case, verbatim.
picodrive has carried upstream's idle-loop whitelist all along -- and wired
it to Cyclone and FAME only, by patching the branch opcode. **gwenesis
(`EMU_G68K`), which is what this build runs, was never wired to any of it**,
and the 32X-register poll detect that does call `SekSetStop` cannot see this
loop because the address polled is RAM, not a 32X register.

`GNW_M68K_IDLE_FOLD` (picodrive `2a28aed8`) detects at the branch instead of
patching: a taken short backward `Bcc`/`BRA` whose body passes
`SekIsIdleCode`, verdict cached per branch target so the check runs once per
site and never inside the loop. It sets the stop flag directly rather than
going through `SekSetStop`, whose `SekEndRun` rebases `Pico.t.m68c_cnt` --
rewinding the master clock is not what a spinning guest does. It never
sleeps with interrupts masked at or above VInt's level.

    rig  9,901,391 -> 9,630,064 host insn/frame   -2.74%
    68K guest insns 8,845/frame -> 2,195/frame    -75.2%

Framebuffer hashes bit-identical, guest SH-2 instruction count identical.
**Device verdict pending** (arm `idl1`).

**The audio hash moves, and that needed a new instrument to interpret.**
`af8c118b -> 8c20ba95`, on a pixel-identical frame. The VInt handler starts
up to one scanline later, so every sample boundary downstream shifts. The
rig now also reports audio *energy*, which survives a time shift and does not
survive a change of content:

    sum|s|    37,682,184 vs 37,690,020    -0.021%
    sum s^2   231,270,959,840 vs 231,289,673,356   -0.008%
    peak      32767 vs 32767              identical

Energy does not replace the hash -- an unmoved hash is still the only proof
of bit-identity -- but the pair separates "rescheduled" from "broken".

After the fold the 68K's hottest code is a *called helper*, not a spin:

    0x882d6a  move.w (a2), d3
    0x882d6c  cmp.w  (a2), d3
    0x882d6e  bne.b  $882d6a        ; read a volatile word twice until it agrees

79 calls a frame, exits on the first compare almost always. Do not fold it:
the second read is the point.

## Closed today, by measurement

- **NOP short-circuit.** The opcode census (`RIG_OPHIST`, picodrive
  `e9fcda4b`) says msh2 spends 12.08% of its dispatched instructions in the
  `0x00xx` group, 64.7% of which is NOP -- 7.8% of everything -- and 28.0%
  RTS. Short-circuiting NOP ahead of `op0000`'s 64-way second-level switch
  measured **0.10%**. gcc's jump table for that switch is already cheap; what
  a NOP pays for is the loop iteration, not the sub-dispatch, and removing
  the iteration costs more in the delay-slot path than it saves.
- **Carrying `delay`/`test_irq`/`ppc` in locals** (arm A5): **+3.14%, a
  regression.** The dispatch loop is at its register ceiling; one more live
  value and gcc spills. Do not "optimise" this loop by adding locals.
- **The MD sound subsystems are not free work.** `-DRIG_NO_FM` is worth
  -3.54% and `-DRIG_NO_Z80` -4.30% on the rig, and **both move the audio
  hash**, so Doom 32X genuinely drives the YM2612 and the Z80. There is no
  silent chip to switch off.

## D32XR: the official 5 MiB build boots, and the 41 fps claim does not survive

picodrive `7e04cb69` (SSF2 bank writes wired into GNW builds) is in `HEAD`,
and the official release now runs in the rig: **GATE3 PASS over 500 frames,
framebuffer alive and moving** (`/home/ubuntu/32x_roms/d32xr_official_5m_plain.32x`).

But it is not a shortcut. Per frame the rig measures

    D32XR        31.78 M host insn,  319,009 guest SH-2 insn
    retail Doom   9.88 M host insn,   94,571 guest SH-2 insn

**3.2x the host work and 3.4x the guest work.** The 41.4 drawn fps recorded
for D32XR on 2026-08-15 -- against retail Doom's 30.6 in the same session --
is arithmetically impossible against those numbers, and the handoff already
carries the discriminator for what it probably was: *60 fps with a static 3D
scene means the guest parked itself*. If D32XR is benched on the device
again, take two screenshots seconds apart and prove the picture changes
before quoting a frame rate.

---

## State

| | |
|---|---|
| Retail Doom, attract anchor | **21.79 drawn fps** (1800-frame window, 5 samples, spread 0.16) |
| Retail Doom, before today | 7.85 — the forced-draw ratio fix is worth **×2.8 on the same anchor** |
| Retail Doom, **gameplay anchor** (savestate resume) | **16.10/16.09 drawn fps** (measured 2026-08-16, screenshot-verified first-person scene) |
| Retail Doom, gameplay anchor, 08-17 tree | **15.89/15.91/15.89** (1800-frame ×3, savestate-resume arm `gpf` = 08-16 tree + the PWM-read sync restore; spread 0.02). Same anchor as above; the ~0.2 delta is the tree change |
| Retail Doom, gameplay anchor, OC A/B (08-17) | `MD32X_OC_LEVEL=1` **15.19×3** vs `=2` **17.02×3** (arms `oc1g`/`oc2g`, same committed tree, intflash byte-identical, only `32x.bin` differs) — **+12.0%, spread 0.00**. Reverses the attract-demo verdict (+0.6%); see `32X_CLOSED.md` clock-floor section. **Default raised to 2** (8653e579); default-flag build re-verified **16.94/16.95/16.95** (arm `ocdef`) |
| Retail Doom, fps axis 08-19 | **17.49 (v23) → 21.39 (five levers, arm `ahle`) → 24.02 (sound HLE, arm `hle4`) = +37% cumulative**; blank-row cache on top measured **23.66×3 = −1.46% device regression** (same-session A/B vs re-benched `hle4` 24.01×3; rig predicted −1.82% gain — sign crossed) and was **reverted** (picodrive `dc0e0b7f`+`48d3f5b6`; lesson now a rule — 32X levers need a device drawn bench before shipping, see `32X_CLOSED.md` Ledger) |
| D32XR (4 MiB bench build) | old "41.4 drawn fps" claim is **from a tree state no longer reproducible** — on 2026-08-15's tree D32XR deterministically HardFaulted at frame 59. Two bugs fixed 2026-08-16 (below); now boots and renders, but dies to `Z_Malloc: failed on 496` (open) |
| Real gameplay speed | retail Doom measured (16.1); D32XR pending its Z_Malloc fix |
| Emulated speed | ~36% of a 60 Hz machine (retail Doom attract). The console still plays in slow motion |
| Branch | `testbed`, pushed through `be6dc79b`. Submodules `external/sm` @ `5bc1605`, `external/picodrive` @ `c06b334e` (gnw-port), both on remotes |
| Arms built | `/tmp/gnw_arms/oc1`, `/tmp/gnw_arms/oc2` (cold-boot autoboot, **not** savestate-resume); `/tmp/gnw_arms/{gp,gpr,gpf,gp2,gp3,oc1g,oc2g}` (savestate-resume, 08-15→08-17 campaigns) |
| Worktree | `exp/32x-oc` — a scratch worktree used to build away from a checkout whose `external/sm` was mid-surgery |

## What changed today

**D32XR runs on the console.** Eight hours went into "which of our deltas broke
it" and the answer is none: it wedges only in `tools/m7_qemu_rig`. The screen was
verified, not inferred — see `tools/gnw_probe/screenshot.sh`. Details and the
withdrawn suspect list are in `32X_CLOSED.md` §0b.

> **2026-08-16 correction: the "wedges only in the rig" conclusion above was
> wrong.** On the then-current tree D32XR froze on the *device* too, 3/3 boots,
> at frame 59 — the same fault the rig showed. Two fork bugs were found and
> fixed (picodrive gnw-port `c06b334e`):
>
> 1. **STRD HardFault at first render.** `do_line_pp`'s solid-run blast wrote
>    two adjacent `u32` stores of the same register; gcc fused them into `STRD`,
>    and `DrawLineDest` can be 2 mod 4 — a faulting 64-bit access on Cortex-M7
>    (and QEMU's M33). Same class as Super Metroid's `ClearBackdrop`. Fix:
>    align the destination pixel first, then u32-blast.
> 2. **PWM poll-detect freeze.** A fork-added `p32x_sh2_poll_detect` on PWM
>    register reads, but PWM writes never fire `p32x_sh2_poll_event` — so the
>    slave SH-2, polling PWM CH3 during init, was CPOLL-frozen with no possible
>    wake (its `sh2irq_mask` is CMD-only). Fix: restore the upstream plain read.
>
> Device-verified after both fixes: D32XR boots and renders (69k/76.8k nonzero
> px, both SH-2s alive and advancing); retail Doom regression check on the same
> arm: 17/17 emu/drawn fps (baseline 16.1). The QEMU rig runs D32XR 200 frames
> GATE3 PASS — same rig, same ROM, now clean.

**The clock floor is not a lever — on the attract demo.** The paragraph below
was written from attract numbers; the 08-17 gameplay A/B reversed it: **+12.0%**
(15.19×3 → 17.02×3). Closed with numbers in `32X_CLOSED.md` §"clock floor".
The default is now **2** (8653e579, 340 MHz) — re-verified through a
no-flags build at 16.94/16.95/16.95.

**The 10–25 min unattended wedge is the game parking its own 68K.** Long
unattended soaks after a clean bench end in a state that *looks* like a
deadlock: 3D scene frozen, counters still pumping ~60 fps, msh2 pinned in the
SDRAM "flow" poll, ssh2 in its `bra-self`. It is not a lost wakeup — sampled
30×2 s in that state, the emulated **m68k PC is pinned at `0x8808a8`, a
`bra-self` in the ROM's own park block** (`move #$2700,sr`, interrupts off;
ROM dump + capstone, mirror at `0x880000`). The 68K vector table routes every
exception to a *different* park block (`0x8808aa` → parks at `0x8808c0`), so
the observed address is a **called** clean-shutdown, not a crash handler.
Downstream symptoms (SH-2 waits, cheap 60 fps frames) are consequences, not
causes. Not a picodrive bug, not clock-related (identical state at 312 and
340 MHz), not the PWM sync restore. Discriminator for future soaks:
**60 fps + static 3D + m68k PC pinned on a `bra-self` = the guest parked
itself — reset before measuring.**

## The anchor — the most important open item

**Every 32X fps this project has published, today's included, is the attract
demo behind the title menu.** That was confirmed by photographing the
measurement window mid-bench. It is real 3D rendering, not a still, so the
numbers are not fake — but they are not gameplay either, and the gap is large:
the same ROM measured 18.33 on 2026-08-14 against a heavier window.

There is already a gameplay savestate on the card, an early-game scene:

```
/data/32x/둠 (Doom).32x-0.sav      (+ -0.raw, its preview)
```

Use it. Build with `GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=0` and autoboot
**`둠 (Doom).32x`** — the savestate path is keyed by ROM filename, so the
`doom.32x` pushed today has no save and will start cold. Then re-run the OC A/B
against it: a heavier load weighs core clock against OSPI differently, and that
is the one thing that could overturn the result above.

## The queue

0. **D32XR `Z_Malloc` — ROOT CAUSE FOUND AND FIXED (2026-08-18, third rewrite;
   picodrive `7e04cb69`).** The official 5 MiB release now boots to the DOOM
   title screen in the rig. The 4 MiB bench cut still dies on 496 by a separate
   mechanism, see the tail of this entry.

   **The 5 MiB chain, every link observed on the rig** (`-DRIG_LM_TRACE` +
   `-DRIG_WALK_TRACE`, f49, logs `/tmp/opencode/lm5.log`, `bnkw1.log`):

   1. The SH-2 lump cache is a sliding-window mapper. On a miss its callback
      (`0x06006bd0`) asks the 68K over comm0 to program the cart bank window:
      `comm0 = 0x1600 | bank<<3 | slot` — observed `0x163E` (slot 6, bank 7).
   2. The 68K command handler (blob `$ff103c`, disassembled from ROM file
      0x4580+) does `move.b #bank, $a130f0 + 2*slot + 1` → `$a130fd = 7`.
      The write was *observed reaching the emulator* — `[bnkw] a=00a130fd d=07` —
      and `carthw_ssf2_banks` stayed `00..07` identity anyway.
   3. Because the bank register write was dropped, every windowed pointer read
      one 512 KiB bank low: the cache returned `0x2233ba68` (window 6) for
      TEXTURE1, the LZ decoder at `0x02018b68` read garbage (`"aa"` +
      backreference underflow past the destination), so the expanded header's
      first u32 was `0x6161` = 24929 "textures".
   4. `24929 * sizeof(texture_t=32) + 24 = 797752` → `Z_Malloc: failed on
      797752`, character for character with the device report.

   **The bug was in the emulator, not the ROM.** `PicoMemSetup32x` installs
   io write handlers under `#ifndef GNW_32X_CORE`, so GNW builds always got
   the *plain* handler, which drops every `$a130xx` write except f1. The fix
   (`7e04cb69`): compile `PicoWrite8/16_32x_on_io_ssf2` in GNW builds too
   (`carthw_ssf2_write8/16` live in `carthw.c`, compiled unconditionally) and
   route to them when `carthw_ssf2_active`. Verified: GATE3 PASS at 320/500/
   4000 frames, avg sh2 332230 insn/frame (was 0 after death), f3999
   framebuffer rendered to PNG = DOOM title screen with menu.

   Note the game never touches the SSF2 bank registers *directly* — an early
   hypothesis ("game programs banks, GNW drops them") was rejected on exactly
   that observation, then restored in the correct form: the game programs them
   *through the 68K comm proxy*, which the plain handler also starves. A
   second proxy path (opcode 0x01, `$a130f1` strobe 3/2 + a byte read at
   `$200000+2*comm1`) answers 0 — that window is still unmapped for 68K reads,
   but boot proceeds past it; it is not the killer.

   **The 4 MiB bench cut (`failed on 496`) is a different death.** Death-stack
   (`RIG_DEATH_STACK=450`, `/tmp/opencode/ds4.log`): the failing allocation is
   a 472-byte lump cache request (`496 = 472 + 24`) from
   `W_CacheLumpNum` (`0x0201edd0`) — plain zone exhaustion, not a count×32
   path. This build is on the ≤4 MiB route (wadbase 0x02036000, every lump
   pointer under the direct-return threshold), so banking is not involved and
   the fix above does not change it. **What filled the zone is not traced —
   uninvestigated, not excluded.**

   *Corrections this cost, kept honest:* the SRAM A/B of the superseded entry
   below "passed" on lying gates; read correctly it says death was identical
   under zero/FF/random SRAM — SRAM is not an input, which the root cause now
   explains (the failure was a mapper wiring gap). The instrument lesson
   stands: a checksum cannot read.

0b. *(superseded verdict, forensics retained)* **`Z_Malloc: failed on 496` —
   device-only; NOT reproducible in the rig.** The line above this
   entry used to say "Repro is deterministic offline: the QEMU rig hits the
   identical screen in 200 frames." **That claim no longer holds and it is the
   most important artifact of the investigation.** What was actually measured,
   separated from what is guessed:

   *Measured (current tree + picodrive 190f6329, ROM md5 110d2229 — byte-equal
   to the card copy pulled 08-15):*
   - The rig does not die. Real BIOS: 4000 frames, GATE3 PASS, framebuffer
     rendering, comm handshakes alive at f3999. Stub BIOS — which is what the
     device actually runs; the firmware tree has zero `p32x_bios` references,
     so `p32x_bios_m/s == NULL` takes the stub path — 4000 frames GATE3 PASS.
   - SRAM variants all PASS too: zeroed baseline, 0xff-filled, random-filled
     (2000 frames each). The game writes nothing to SRAM during boot+title,
     ignores 0xff, but *does* modify 123 bytes of random-filled SRAM — a
     non-zero SRAM is parsed and rewritten by some init path.
   - The `sh2 == 0` seen in old 200-frame runs is a boot-latency artifact, not
     death: SH-2 BIOS copy/checksum occupies f4–f169 (~167k insn/frame) while
     the 68K waits on comm4 (checksum report); handshake completes at f170;
     the MEGASD detect delay loop (~1.5M 68K cycles) runs f181+; game init
     follows. Short windows catch the quiet middle of a long boot and look
     wedged. (This also answers most of old item 4.)
   - What the error *is* (from the ported source, `z_zone.c`): 496 = ~488-byte
     request + 8-byte header, 4-aligned, failing inside the main zone. The zone
     is a compile-time fixed static array in the SH-2 program BSS
     (`BASE_ZONE_SIZE 0x33000` = 208,896 B) — identical on device and rig, so a
     device failure is an allocation-*pattern* divergence during init, not a
     smaller zone.
   - The death screen's own mechanism (from rig forensics of the boot
     sequence): the SH-2 boot program parks polling comm0=="M_OK" (CPOLL). If
     the game's 68K error path resets the SH-2, the BIOS reboot re-copies SDRAM
     and wipes the evidence — the 08-16 device SDRAM dump is post-mortem (no
     zone, no error state; the old ZONEID scan was checking for tags a custom
     allocator never writes).

   *Not known / guessed, deliberately labelled:* why the device diverges. The
   old deterministic repro came from the `exp/32x-d32xr` worktree (3677a674)
   plus throwaway probe builds, not this tree; what changed in between includes
   the D32XR STRD/PWM fixes (c06b334e, 190f6329). **Not bisected** — the old
   worktree is gone, so a bisect would have to recreate it. Remaining suspects:
   real saved-game SRAM on the card, timing, device-only codegen.

   *Leads cancelled, with reasons:* SDRAM-fastpath A/B (the rig never dies, so
   there is nothing to A/B); zone-size dump (the zone is a fixed BSS array, not
   runtime-probed — same answer on both sides).

   **What D32XR actually puts on screen in the rig (measured 2026-08-18, 4 MiB
   bench cut, current tree, 450 frames, `-DRIG_TRACE_CKS`): three static images
   and nothing else.**

   | frames | checksum | what it is |
   |---|---|---|
   | 169 | `00000000` | blank |
   | 75 | `596e0000` | still essentially black (2 colours, dumped) |
   | 206 | `2064b511` | `Z_Malloc: failed on 496` |

   It never draws a game frame. Not a title, not a menu, not a demo — the
   framebuffer takes three values in 450 frames and the last one is the
   allocator error.

   ⚠️ **This does not settle what the 41.4 fps measured, and the two sides
   disagree.** The device log for the run after the STRD/PWM fixes records
   *"boots and renders, 69k/76.8k nonzero px"* — 90% of the screen carrying
   content, which is not a line of white text on black. So on hardware D32XR was
   showing something substantial while the rig shows an allocator failure. One of
   these is not the state the other is in.

   The reason nobody can say which is that **no screenshot was taken at the
   time**. `tools/gnw_probe/screenshot.sh` exists now and did not then. Until a
   device capture of D32XR exists, treat 41.40 drawn fps as a number of unknown
   subject: `drawn` counts LCD flips, and a static screen flips too.

   ⛔ *Do not try to bisect this against `exp/32x-d32xr` @ `3677a674`.* That
   branch still exists and its submodule pin (`external/picodrive 883010c5`) is
   fetchable, so it looks like the known-good end of a regression — the tree that
   measured 41.52/41.47/41.40 drawn fps. It is not. Checked out into a detached
   worktree and run 2026-08-18: the 4 MiB bench build gives a **blank**
   framebuffer, `avg sh2=401`, GATE3 FAIL, and the official 5 MiB release the
   same. It renders nothing at all, because `883010c5` predates the STRD and PWM
   fixes (`c06b334e`) that are what made D32XR boot in the first place.

   The current tree gets strictly further: it renders, then stops on the
   allocator message. **There is no known-good rig state to bisect against**, and
   the 41.4 figure was a device measurement whose on-screen content was never
   captured — worth re-reading in that light before treating it as a target.

   *Device-owner verification, in this order (needs hardware):*
   a. Check the card for a D32XR `.sram` (the ROM header declares SRAM at
      0x200001–0x207fff). Delete it, cold-boot. The timeline fits: 08-15 20:48
      the title screen was reached (first boot, SRAM empty) — every later boot
      failed. A real saved SRAM is the one input the rig never saw.
   b. Re-verify on current HEAD — the failing observations predate 190f6329.
   c. If it still reproduces: dump SDRAM *at* the failure moment, before the
      SH-2 reboot wipes it (SWD halt + mdw, not a post-reboot dump).

   *Probe persistence:* the rig probes are now in-tree —
   `tools/m7_qemu_rig/run_32x.sh` + `RIG_SH2_WATCH` (68K PC track + SH-2
   SDRAM→BIOS edge), `RIG_STRPAGE` (68K page-0x8a string-reader trap),
   `RIG_SDRAM_SCAN` (post-death SDRAM scan), `RIG_SRAM_FILL`
   (preload/fill/dump; note QEMU semihosting passes no env vars and SYS_OPEN
   returns -1 — the knobs are compile-time macros, see HARNESSES.md),
   `RIG_DEATH_STACK` (sh2 regs/stack + SDRAM windows + 64 KiB 68K RAM dump,
   via `cart.c rig_pico_ram()`), `RIG_LM_TRACE` (comm-protocol writes from
   both sides) and `RIG_WALK_TRACE` (directory walk / cache-pointer /
   decoder-read / bank-register taps) — the last two and the death stack are
   what ran this entry's forensics (picodrive `a39ce414`, super `1ec73241`).

1. **Gameplay-anchored numbers for everything.** Retail Doom **done** —
   16.10/16.09 (2026-08-16), 15.89/15.91/15.89 on the 08-17 tree, and the OC
   A/B **done 2026-08-17**: L1 15.19×3 vs L2 17.02×3 = **+12.0%** — the clock
   floor *is* a lever in gameplay (see `32X_CLOSED.md`); whether to raise the
   default is a stability/battery call, not a measurement one. Remaining:
   D32XR (blocked on item 0).
2. **Device PC profile — DONE 2026-08-17** (gameplay anchor, savestate-resume
   arm, 2000 host-PC + 2000 guest-PC samples). Host: `sh2_execute_interpreter`
   dispatch **49.4%** of samples, SH-2 family ~60.8% total — July's "half the
   frame is SH-2 dispatch" holds in gameplay. Guest: msh2 sits 66% in three
   loops — 36.5% `R_DrawColumn` (texture column render, real work), 23.4% an
   SDRAM flag poll at `0x06001170` (Doom's 68K↔SH-2 "flow" handshake; woken by
   VINT IRQ, verified by watchpoint: nobody else writes the flag at runtime),
   6.2% software 32-bit division; ssh2 97% in a `bra-self` park plus a 5.5% PWM
   mono-FIFO poll. Follow-up built and rejected the same day: parking the SDRAM
   poll (see the ledger in `32X_CLOSED.md`) — detect cost on the read fastpath
   outweighs the spin it saves.
3. **A cart over 4 MiB now banks — DONE 2026-08-18, at a measured cost.** The
   only thing that had been compiled out under `GNW_32X_CORE` was
   `carthw_ssf2_startup()`, the standard large-ROM bank mapper. Everything else
   was already there: `pico/32x/memory.c` has had bank-aware SH-2 read paths and
   the `$a130xx` write handlers all along, running against two stub symbols that
   nothing ever set. So the official 5 MiB D32XR release was unrunnable for the
   want of one call.

   *Priced, as this entry used to ask.* `carthw.c` is ~7.6 KB of text+rodata,
   which sounded fatal against 1,236 B of `.overlay_md32x` headroom (the overlay
   was at 99.83%: 31,932 + 708,208 of 741,376). It is not, because the linker
   script claims only `.data` from `build/md32x/*.o` and sweeps every other
   object's text and rodata into `.xip_md32x` — external flash. **Measured cost:
   +24 B overlay, +16 B BSS, 40 bytes total.** Bank writes are I/O-rate, so XIP
   is the right home for them.

   *Measured behaviour (QEMU rig, 600 frames, D32XR 4 MiB card image vs the same
   image zero-padded to 5 MiB so the fallback fires):*

   | | 4 MiB | 5 MiB |
   |---|---|---|
   | gate | GATE3 PASS | GATE3 PASS |
   | fb hash f299 / f599 | `2064b511` | `2064b511` — identical |
   | avg host insn/frame | 6,960,598 | **8,789,308 (+26.3%)** |

   Bit-identical frames: turning the mapper on does not perturb a game that does
   not bank. The +26% is not the mapper's own work — it is
   `gnw_sh2_rom_fetch_mask = carthw_ssf2_active ? 0 : gnw_rom_map_mask`
   (`pico/32x/memory.c`). The cart-ROM opcode-fetch fast path is a plain
   `MAP_MEMORY(Pico.rom)` mirror, which stops being true the moment a bank can
   move, so it switches itself off and every fetch pays the cross-TU
   `p32x_sh2_read16` call instead. Doom's msh2 executes 100% out of cart ROM,
   ~173k fetches a frame.

   *And then the real ROM turned up.* It was on this machine all along, under
   `/tmp/opencode/wt32xp/*/rom.32x` (5,242,880 B, md5 `2a23fcf6…`), left by an
   earlier session. **The official 5 MiB D32XR release now loads and starts**,
   which it had never done here:

   ```
   00000:000: SSF2 mapper startup
   [32x-qemu] romlen=5242880 ssf2_active=1
   00002:091: 32X startup
   ```

   Two traps on the way, both worth keeping:

   - **That copy is byte-swapped and the 4 MiB card image is not.** Its header
     reads `ESAGS FS` where the card image reads `SEGA 32X`. The rig wants the
     plain form; fed the swapped one it runs the 68K at 926k insn/frame with
     `sh2=0` and a blank screen — indistinguishable from a wedge. Check the
     header before believing any 32X result: `dd bs=1 skip=256 count=48 | strings`
     should say `SEGA`, not `ESAGS`.
   - **The system field of the real cart is `SEGA SSF`, not `SEGA 32X`** — the
     SSF2 signature sits where the 32X one does on the bench build. 32X still
     enables (it comes from the game's ADEN write, not the header), but anything
     that keys off that field will disagree between the two ROMs.

   **Where it stands now — a device-only failure became an offline one.** With
   the real cart the mapper installs, 32X starts, the framebuffer is non-blank
   (`c8b3777c`) — and it never changes again, from f99 through f1999, while the
   SH-2s run ~1.4–2.6k insn/frame against the bench build's 61k. They are alive
   and parked. The `Z_Malloc` string trap fires zero times
   (`RIG_STRPAGE_ADDR=0x8a4c9c` — the string moved from `0x8adbdc` in the bench
   cut, and a trap pinned to the old address reports "0 hits" while watching
   nothing, so it is a knob now).

   That is a different failure from the one item 0 chased, it is deterministic,
   and it needs no device. **Next session starts here, not on hardware.** First
   questions: does the SH-2 see the banked window correctly (`memory.c:1536+`
   claims to handle it), and what is it polling when it parks?

   **Open, and the obvious next lever:** make the fetch fast path bank-aware
   rather than off — `Pico.rom + (carthw_ssf2_banks[(a >> 19) & 7] << 19) +
   (a & 0x7ffff)`, which is the same arithmetic `memory.c:1615` already does on
   the slow path. That is a hot-path edit and it was NOT attempted here, because
   the real 5 MiB D32XR release is not in this tree and a synthetic pad never
   exercises a bank switch. Get the real ROM first; it is also the only way to
   learn whether >4 MiB was ever the thing keeping it from running.
4. **Rig fidelity — mostly answered 2026-08-18, remainder low priority.** The
   old "the rig wedges" observation was the boot-latency artifact described in
   item 0: 200-frame windows sit inside the quiet middle of a ~300+-frame boot
   (BIOS copy → MEGASD detect delay → init) and read as `sh2 == 0`. Run 4000
   frames and the same rig boots, renders and handshakes to the end. What is
   still unknown about fidelity is only the pacing mapping (rig icount vs
   device wall-clock), which nobody currently needs. The rig counts
   instructions; it does not decide anything. ⛔ Do not try to settle pacing by
   linking the upstream tree inside the rig — the stub chase does not terminate
   and the result is a third program.

## Where a frame actually goes (rig, 2026-08-18)

First host-side phase breakdown. Retail Doom, **attract** anchor (no savestate —
so this is the light window; treat the shares, not the absolutes, as the
guidance), 600 frames, `EXTRA_DEF=-DRIG_PHASE_PROF`:

| phase | insn/frame | share |
|---|---|---|
| msh2 (interp+bus) | 3,898,834 | **49.5%** |
| ssh2 (interp+bus) | 1,352,060 | 17.1% |
| 32x compositor | 841,787 | 10.6% |
| draw (MD VDP line) | 724,669 | 9.2% |
| m68k | 543,001 | 6.8% |
| fm / snd / z80 / pwm | 386,682 | 4.8% |
| other (sched/ev/mem) | 129,173 | 1.6% |
| **PicoFrame total** | **7,876,239** | 100% |

**The number to aim at is not in that table: `sh2 host/guest = 75.14`.** Every
guest SH-2 instruction costs seventy-five host instructions. Two thirds of the
frame is SH-2, so that ratio *is* the core's speed. For scale: getting 75 down
to 45 would take the frame from 7.88M to ~5.8M, about −27%, without touching a
single guest cycle. That is the largest lever anyone has priced here, and the
tree already has the precedent for it — the SNES core runs a hand-written
Thumb-2 65816 and SPC700 for exactly this reason.

⛔ **And do not "simplify" the fastloop pre-filter out of the dispatch.** It
costs ~7 host instructions on every guest instruction and looks exactly like the
add-a-test-to-skip-work shape this tree normally loses on. Priced by ablation
(`-DGNW_SH2_NO_FASTLOOPS`, picodrive `8023c183`): removing it takes the frame
from 7,876,239 to **27,488,194** insn — 3.5x worse — because the guest
instructions actually interpreted go from 69,878 to 434,967. The filter is
buying the loops the game lives in. Note the trap in the ratio: host/guest
*improves* from 75.1 to 57.2 when you remove it, which is a good reminder that
host-per-guest is not a figure of merit by itself — it gets better when you make
the guest do more work.

### Why idle-skip breaks the sound — mechanism, measured 2026-08-18

The old verdict was "it broke Doom's gunshot PWM SFX", found by ear on hardware.
The rig can hear now (audio hash, `dfa8678d`) and the mechanism is this:

| arm | insn/frame | framebuffer | audio hash |
|---|---|---|---|
| baseline | 7,694,031 | 9cd2510f/22ee77a6/ced1080b | `f4c01e1e` |
| idle-skip | 6,329,078 (−17.7%) | identical | **`3ac9f381`** |
| idle-skip + wake on PWM FIFO movement | 7,662,594 (−0.4%) | identical | **`f4c01e1e`** ✓ |

A parked SH-2 is only woken by an internal IRQ (`sh2_internal_irq`); `pwm.c`
contains no wake at all, and the VBlank `p32x_sh2_poll_event` excludes
`SH2_STATE_SLEEP` explicitly. Doom's **slave SH-2 does not wait on the PWM
interrupt — it busy-polls the mono FIFO count**, so once parked it never sees
the FIFO drain and never refills it. Waking it on FIFO movement restores the
baseline audio hash exactly, which confirms the mechanism.

And then the gain is gone: −17.7% becomes −0.4%, whether the wake fires on every
consumed sample or only when the FIFO empties. **ssh2's 17.1% of the frame is not
idle spin — it is the sound service loop**, running at audio rate. The spin *is*
the waiting, and skipping the waiting skips the sound.

So the old verdict stands, but for a better reason than "it broke SFX", and the
next attempt has a gate that catches it offline in one run instead of a device
round trip and an ear.

**What this does leave open:** neither existing collapse handles a poll on a
*PWM register*. The SDRAM-poll detector deliberately restricts itself to
`0x06000000` targets (an `RW()` on a sysreg/comm address would fire poll_detect
and corrupt the guest's poll state), and the bra-self path is a different shape.
A detector that understands the PWM FIFO could compute when the count will next
change and skip to exactly there — cycle-exact, so the audio hash would have to
stay `f4c01e1e`. That is the one untried shape on this axis.

⛔ **Do not re-derive the idle-skip lever from this table.** ssh2's 17.1% is
almost entirely a `bra-self` park, and switching on the existing
`gnw_sh2_idle_skip` removes it: measured here 7,876,239 → 6,511,024 insn/frame,
**−17.3%, with all three framebuffer hashes identical**. It is still closed, for
reasons recorded in `main_md32x.c` and `32X_PERFORMANCE_RESULTS.md` 측정10: on
the **device** it measured **0 fps effect** (rig instruction savings did not
translate to device cycles), its CRC32 whitelist matched one dump, and it broke
Doom's gunshot PWM SFX — which no framebuffer hash can see. A clean rig A/B with
identical frames is exactly what this lever looked like the last time too.

### What is already optimised — priced by ablation 2026-08-18, do not re-derive

Four levers were re-examined in one sitting and **all four turned out to be
already done or already closed**. Each was priced by building both arms and
checking the two binaries actually differ, because two of them first showed a
delta of exactly zero:

| lever | status | measured |
|---|---|---|
| SH-2 fastloop filter | **already on** (`sh2pico.c` defines it unconditionally) | removing it: 7.88M → 27.49M insn/frame, 3.5× worse |
| BF/S countdown collapse (`TST Rn,Rn` / `ADD #-1,Rn`) | **already implemented** | verified firing at Doom's hottest loop, `r4=0x31`, 48 of 49 iterations collapsed |
| SH-2 idle skip | ⛔ closed on the device | rig −17.3%, device 0 fps, broke Doom's PWM SFX |
| packed-pixel solid-run detector | **already on and paying** | removing it: compositor 773k → 945k insn/frame, +22% |

The two zero-deltas were the instructive part. `-DGNW_SH2_FASTLOOPS` changed
nothing because the feature was already unconditionally enabled, and a
hand-written BF/S countdown collapse changed nothing because an identical
handler already sat earlier in the same function. Both would have been reported
as "no gain" if the arms had not been md5'd first.

Ablation switches were left behind so the next person re-prices in one build
instead of deleting code to find out: `GNW_SH2_NO_FASTLOOPS`, `GNW_PP_NO_RUNDET`.

**The MD VDP renderer was the last unexamined bucket, and its ceiling is low.**
`GNW_MD_ABLATE` (picodrive `6b403f94`) kills every MD layer so the bucket
collapses to what is irreducible: draw 724,669 → 345,920 insn/frame, whole frame
7,700,620 → 7,279,779, **−5.5% for deleting the layer entirely**. Two thirds of
its cost is per-line overhead that survives having nothing to draw. And it is not
deletable: the framebuffer hashes change (f99 and f299 go blank), so Doom's MD
layer carries visible content, not merely the background mask the compositor
tests. Any real optimisation there is a fraction of 5.5%.

**Where that leaves the frame.** Retail Doom, attract, 400 frames: the
compositor is 10.0% at ~10.8 host instructions per pixel, MD VDP draw is 9.2%,
and two thirds is SH-2 executing the game. The interpreter-level levers are
spent. The MD VDP renderer has now been priced too (above), and it is
not the lever it looked like. **Everything cheap is spent.** What remains is
structural: the SH-2 interpreter itself, ~66% of the frame, where the tree's own
precedent is the SNES core's hand-written Thumb-2 65816 and SPC700. That is a
project, not an afternoon, and it should be entered with an ablation estimate of
what a faster dispatch is actually worth on the *device* — the idle-skip lesson
is that rig instruction savings do not automatically become device fps.

### Dispatch-prologue ablation — the estimate the paragraph above asked for (2026-08-18)

The interpreter's dispatch was taken apart two ways: statically, from the A0
ELF's `sh2_execute_interpreter` disassembly, and dynamically, by building
single-knob ablation arms in a throwaway lane and running the gameplay anchor
(retail Doom, savestate resume, 900 frames, workload identity verified by
identical `avg sh2 = 94,406` dispatched/frame on every valid arm; every arm
md5'd against baseline first). **The measurement builds were never committed —
that is the standing rule for ablation arms.**

*Static, per dispatched guest instruction (direct path), host instructions
around the handler body:*

| stage | insn |
|---|---|
| delay-slot check | 3 |
| pc load + ppc store | 2 |
| fetch fast path (two masks, direct load) | 11 |
| fastloop pre-filter, interleaved with state updates | 11 |
| dispatch jump (sp table: add/lsl/ldr/orr/bx) | 5 |
| handler stub (`mov r0,r4; bl opNNNN; b back`) | ~3 |
| loop tail (icount--, irq check, both loop conditions) | 11 |
| **total around the handler** | **≈52** |

Plus ~15 host instructions once per *slice* (the 16-word stack dispatch-table
copy) — dozens of times a frame, negligible. Against the measured
`host/guest = 75.14`, the handler bodies themselves are only ~23 of ~75 host
insns: **the scaffolding around each guest instruction is ~2/3 of interpreter
cost.**

*Ablation, avg host insn/frame (lower is cheaper):*

| arm | change | avg host | delta |
|---|---|---|---|
| A0 baseline (a39ce414 clean) | — | 9,802,331 | — |
| A2 dispatch table → rodata | slice copy removed, PC-relative load | 9,712,214 | **−0.92%** |
| A3 switch dispatch (replaces computed goto + stubs) | compiler jump table | 9,590,466 | **−2.16%** |
| A4 no fastloop pre-filter | filter + fastloop removed | 28,214,186 | **+187.8% (2.88×)** — dispatched inflates 94k→442k; the filter is buying exactly the loops the game lives in. Confirms `8023c183` (3.5× on attract). Keep it. |
| A5 no IRQ delivery | tail `if(0)` | 31.9M | **invalid measurement** — IRQ never delivered, workload diverges; priced nothing |
| A5b tail drops `!delay` half of irq check | `if(test_irq)` | 9,704,547 | **−1.0%**, semantics unproven (irq taken during a pending delay slot) — measured, not proposed |

**What this answers, question by question.**

1. *Where the dispatch prologue goes:* the table above — fetch fast path and
   the interleaved filter+state-update block are the two 11-insn items; the
   dispatch jump itself is only 5.
2. *What can actually be shaved today:* semantics-identical ceiling measured at
   **A3+A5b ≈ −3.2% host cost**, of which A3 alone (switch beats the
   hand-rolled stack-table computed goto — GCC's rodata jump table is cheaper
   than `add/lsl/ldr/orr/bx` off sp) is −2.16% and trivially shippable. A2's
   −0.92% is subsumed by A3.
3. *Device translation:* SH-2 family is 66.6% of the device frame; a ~3%
   whole-frame rig saving is ~+0.5 fps at 16.95. Real, cheap, not a game
   changer. The structural target is different: if the ~52-insn scaffolding is
   ~60% of interpreter time ≈ **~40% of the whole frame**, then a Thumb-2
   dispatch core that *halves* the scaffolding is worth ~20% of the frame —
   16.95 → ~21 fps. That last number is an **estimate** (static counts over
   the measured 75.14 host/guest), not an ablation; the ablated part is only
   the −3.2% ceiling of re-arranging the existing C.

Lane/protocol detail for whoever re-runs: lane `pd2` = clone of picodrive
`a39ce414`, only `cpu/sh2/mame/sh2pico.c` edited per arm, restored with
`git checkout` after; arms md5'd (`8e5daf6d` baseline, `57e00101` A2, …);
`avg sh2` equality is the workload-identity gate — A5 shows why (its game
broke, and its 3.26× "cost" was the breakage, not the tail check).

⚠️ And do not instrument `FinalizeLine32xRGB555` looking for the compositor. Its
own comment says "almost never used (Wiz and menu bg gen only)"; Doom composites
through `PicoDraw32xLayer`, in packed-pixel mode (`Mx=1`, H40, 224 lines,
`Pico32xDrawMode=1`, no scan hooks) — confirmed by instrumenting it, after a
probe in the other function printed nothing at all.

## How to measure this core without wasting a day

Four things cost real time on 2026-08-15. All four are cheap to avoid.

- **1800-frame windows, never 900.** At 900 the same arm read 26.84 / 26.89 /
  29.18 — a spread the size of the effect under test. At 1800 it reads
  21.77–21.93. Short windows sit on the light part of the demo and read
  optimistically.
- **Hash the card's `/cores/32x.bin` against the arm's.** Every `MD32X_C_DEFS`
  knob leaves both arms' intflash byte-identical, so `drawn_ab.sh`'s flash-side
  check passes for either one. Its card-side check was silently skipping until
  today (`arm32x.sh` wrote the core to the arm root, `drawn_ab.sh` looked under
  `cores/`). It printed `SKIPPED` in every run and went unread for a whole
  bench: **grep the output for it.**
- **Photograph the measurement window.** `screenshot.sh --live` takes ten
  seconds and settles "which scene was that" permanently. Half a day of numbers
  was untrustworthy for want of one image.
- **Reset before measuring after any idle period.** An unattended device that
  has been sitting in Doom for 10–25 minutes may have parked its own 68K (see
  the wedge note above) — the counter then reports a meaningless ~60 fps. One
  `reset run` (or any bench script's built-in reset) restores the real anchor.
- **Build in a worktree.** `build/` is shared across every session in a
  checkout, and `external/` can be mid-edit by someone else. Two builds died
  that way today — one on a truncated object left by a killed build, one on
  another session's in-flight work.
