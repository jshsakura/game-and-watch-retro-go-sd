# Sega 32X — where this stands, and what to aim at next

Written 2026-08-15, at the end of a day that produced one large win, one large
correction, and one axis closed by measurement. Read `32X_CLOSED.md` §0 and §0b
first; this file is the working state and the queue, not the ledger.

Everything below is measured on hardware unless it says otherwise.

## State

| | |
|---|---|
| Retail Doom, attract anchor | **21.79 drawn fps** (1800-frame window, 5 samples, spread 0.16) |
| Retail Doom, before today | 7.85 — the forced-draw ratio fix is worth **×2.8 on the same anchor** |
| Retail Doom, **gameplay anchor** (savestate resume) | **16.10/16.09 drawn fps** (measured 2026-08-16, screenshot-verified first-person scene) |
| Retail Doom, gameplay anchor, 08-17 tree | **15.89/15.91/15.89** (1800-frame ×3, savestate-resume arm `gpf` = 08-16 tree + the PWM-read sync restore; spread 0.02). Same anchor as above; the ~0.2 delta is the tree change |
| Retail Doom, gameplay anchor, OC A/B (08-17) | `MD32X_OC_LEVEL=1` **15.19×3** vs `=2` **17.02×3** (arms `oc1g`/`oc2g`, same committed tree, intflash byte-identical, only `32x.bin` differs) — **+12.0%, spread 0.00**. Reverses the attract-demo verdict (+0.6%); see `32X_CLOSED.md` clock-floor section. **Default raised to 2** (8653e579); default-flag build re-verified **16.94/16.95/16.95** (arm `ocdef`) |
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

0. **D32XR `Z_Malloc: failed on 496` — device-only; NOT reproducible in the rig
   (rewritten 2026-08-18 after a full offline campaign).** The line above this
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
   returns -1 — the knobs are compile-time macros, see HARNESSES.md). The
   `RIG_LM_TRACE` reg-write/reset hooks were throwaway in /tmp and are gone;
   recipe to rebuild: printf in `p32x_reg_write8/16`, `p32x_sh2reg_write16`,
   `sh2_reset`.

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
