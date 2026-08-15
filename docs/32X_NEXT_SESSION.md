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
| D32XR (4 MiB bench build) | **41.4 drawn fps** on the title/attract anchor, +35% over retail Doom there |
| Real gameplay speed | **not measured yet.** Every number above is the attract demo. See "The anchor" |
| Emulated speed | ~36% of a 60 Hz machine. The console still plays in slow motion |
| Branch | `testbed`, pushed through `5d337946`. Submodules `external/sm` @ `2f81f9cb`, `external/picodrive` @ `883010c5`, both on remotes |
| Arms built | `/tmp/gnw_arms/oc1`, `/tmp/gnw_arms/oc2` (cold-boot autoboot, **not** savestate-resume) |
| Worktree | `exp/32x-oc` — a scratch worktree used to build away from a checkout whose `external/sm` was mid-surgery |

## What changed today

**D32XR runs on the console.** Eight hours went into "which of our deltas broke
it" and the answer is none: it wedges only in `tools/m7_qemu_rig`. The screen was
verified, not inferred — see `tools/gnw_probe/screenshot.sh`. Details and the
withdrawn suspect list are in `32X_CLOSED.md` §0b.

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

1. **Gameplay-anchored numbers for everything.** Retail Doom, D32XR, and the OC
   A/B. Until this exists, no 32X figure in any document describes play.
2. **Device PC profile — this is the one that matters.**
   `tools/gnw_probe/gnw_probe.sh sample <arm>/gw_retro_go.elf 2000`, on the
   gameplay anchor. It has never been run against the current build. July's cost
   model says a frame is 24.4 M cycles, msh2 is 68% of it, and 75% of an msh2
   instruction's 94 cycles is interpreter decode/execute already running from
   ITCM — so roughly **half the frame is SH-2 dispatch**. The open question is
   which functions, and whether gameplay keeps that ratio.
   Resolve overlay addresses (`0x24xxxxxx`) through that arm's
   `build/gw_retro_go.map` filtered to `build/md32x/*.o`; every core's overlay
   shares the VMA and gdb will happily name another core's symbol.
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
