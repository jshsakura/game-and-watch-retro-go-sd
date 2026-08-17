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
| Retail Doom, gameplay anchor, OC A/B (08-17) | `MD32X_OC_LEVEL=1` **15.19×3** vs `=2` **17.02×3** (arms `oc1g`/`oc2g`, same committed tree, intflash byte-identical, only `32x.bin` differs) — **+12.0%, spread 0.00**. Reverses the attract-demo verdict (+0.6%); see `32X_CLOSED.md` clock-floor section |
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

**The clock floor is not a lever.** `common_emu_auto_oc(1)` → `(2)` is +9% of
core clock and −7% of OSPI, and this core's SH-2 fetches its cart from external
flash. Net +0.6%, inside a 0.7% noise band. Closed with numbers in
`32X_CLOSED.md`. The knob (`MD32X_OC_LEVEL`) stays, default 1.

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

0. **D32XR `Z_Malloc: failed on 496` (open, next session's first item).** After
   both fixes the game boots, renders its init screen, then dies to this
   id-tech allocator error (white text on dark green). Everything about it is
   SH-2-side: the 68K never reads the error string (page-0x8a reroute caught 0
   hits), no ZONEID 0x1d42/0x1d4a11 in SDRAM or 68K DRAM at any frame f10-f140,
   and the string has no instruction-form references in ROM (code is
   RAM-relocated). Failure lands f45+, after the MEGASD flashcart detect
   (f14-f45 — writes 0xcd54→$3f7fa, polls $3f800 for 'MEGASD', benign).
   Repro is deterministic offline: the QEMU rig hits the identical screen in
   200 frames (`build/rig_{fbz2,zd*,sref2,errcatch}` + logs in
   `/tmp/opencode/rig_*.log`; ROM = 4 MiB bench build md5 110d2229). Next
   leads: scan the SH-2 data arrays (`sh2s+0x580` per core) for the zone;
   hook the SH-2 ROM fetch or the VDP text-blit to name the failing caller;
   the allocator is custom (no ZONEID tags), so find `Z_Init`'s zone-size
   computation and check what free-memory answer the fork gives differently
   from upstream (fork stub BIOS / SDRAM fastpath are candidates).

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
3. **A 5 MiB cart cannot be banked on this device.** `pico/cart.c` compiles the
   `romsize > 0x400000` mapper fallback out under `GNW_32X_CORE`, along with the
   whole `carthw.cfg` table. The official D32XR release is exactly such a cart
   and has never been run. Price `carthw.c` against the `.overlay_md32x` budget
   before enabling anything.
4. **Rig fidelity, low priority.** Why the rig wedges is unknown; init matches
   the device, the loop differs only in pacing, `set_out_buffer` period and
   `skipFrame`, all guest-invisible candidates. The rig counts instructions; it
   does not decide anything. ⛔ Do not try to settle it by linking the upstream
   tree inside the rig — the stub chase does not terminate and the result is a
   third program.

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
- **Build in a worktree.** `build/` is shared across every session in a
  checkout, and `external/` can be mid-edit by someone else. Two builds died
  that way today — one on a truncated object left by a killed build, one on
  another session's in-flight work.
