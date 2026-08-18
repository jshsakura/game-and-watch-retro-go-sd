# 32X — where today (2026-08-18) left it

Goal as stated: **Doom 32X playing without lag.** This file is what a new session
needs so it does not re-derive any of it. The queue and the closed-axis ledger
stay where they are (`32X_NEXT_SESSION.md`, `32X_CLOSED.md`); this is the day's
delta and the one open blocker.

## The number that matters

Doom gameplay is **~17 drawn fps against a 60 Hz machine — about 28% of real
speed**, and that is the lag. Closing it needs **~3.5x**.

Everything measurable was measured today, and the total of every remaining axis
is about **2x**, most of it unbuilt:

| axis | ceiling | state |
|---|---|---|
| C interpreter cost/instruction | x1.5 | doc arithmetic (92 -> 45 device cycles is its optimistic floor) |
| hand-written Thumb-2 SH-2 core | **~x1.0** | SNES precedent *measured*: the SPC700 Thumb-2 engine, 2,230 lines of asm, bought **+0.5 fps** |
| dynarec | **impossible** | no Thumb-2 (Cortex-M) backend exists anywhere; ledger, twice re-litigated |
| ssh2 spin, 17.1% of frame | **not available** | it is the sound service loop — proven today, see below |
| msh2 SDRAM poll, 23.4% of msh2 guest | x1.13 | parking built and rejected before (detect cost > spin saved) |
| hot leaf HLE (GBR getter) | ~x1.08 | 4 instructions, ~1,977 calls/frame, 11.3% of guest |
| R_DrawColumn HLE | x1.22 (est.) | **cannot be measured yet** — see blocker |
| compositor / MD VDP draw | x1.02 / x1.05 | already optimised; priced today |
| OC 340 MHz, forced-draw 1-in-1 | applied | +12%, x3.7 drawn |

D32XR is the only multiplicative unknown left, and its 41.4 fps figure is now of
**unknown subject** — see `32X_NEXT_SESSION.md` queue 0.

## The blocker, and it gates everything above

**The rig cannot reach gameplay, so every 32X guest profile ever taken here is of
the attract demo.** That includes the "R_DrawColumn is 36.5% of msh2" figure the
HLE plan rests on — that came from a *device* profile of a savestate-resumed
scene, and nothing offline has ever reproduced it.

Today's pad-script work got as far as the main menu drawn on top of a rendered
level (121 colours, 97,198 guest SH-2 insn/frame against attract's 69,879, peak
frame 23.9M host insn — which matches the ledger's "heavy frame = 24.4M device
cycles"). The renderer runs there. The menu never commits.

What was tried and does not work, so nobody repeats it:

- **START is Doom's pause/menu toggle.** Any script that keeps pressing it cycles
  the front end; alternating START and A landed back on the TITLE at f430.
- Pressing exactly twice (title, then commit) never leaves the title.
- **B and C are inert**: an A-only run and an A-then-B-then-C run give identical
  frame counts to seven digits, from different binaries.
- A-mash reaches the menu and holds it.

**The clean fix is a savestate loader in the rig, not more input timing.**
`rig_32x.c` already has `RIG_STATE_CAP` / `RIG_STATE_TEST` / `RIG_STATE_WARMUP`
and the build already compiles `pico/state.c`. The device card has
`/data/32x/Doom.32x-0.sav`; **it is not on this machine**. Get that file here and
the rig can resume the exact scene the device measures, forever, offline. That
single step unblocks the R_DrawColumn question and every gameplay A/B after it.

## Landed today

- **Audio hashing in the rig** (`dfa8678d`). The framebuffer hash cannot hear,
  and the last spin-skip attempt was reverted for breaking Doom's gunshot PWM
  effect — found by ear on hardware after a rig A/B called it clean.
- **Why idle-skip breaks the sound**, mechanism (`ce0c3b6e`): a parked SH-2 wakes
  only on an internal IRQ; `pwm.c` has no wake and the VBlank poll_event excludes
  `SH2_STATE_SLEEP` by name. Doom's slave SH-2 **busy-polls the PWM mono FIFO**
  instead of waiting on its interrupt, so parked it never refills. Waking on FIFO
  movement restores the baseline audio hash *exactly* — and the gain collapses
  from -17.7% to -0.4%. ssh2's 17.1% is sound work, not idle spin.
- **Five levers re-priced by ablation, all already done or capped** — fastloop
  filter (removing it is 3.5x worse), BF/S countdown collapse (already
  implemented), packed-pixel run detector (+22% without it), MD VDP layers (-5.5%
  only by deleting visible content), idle-skip. Switches left behind:
  `GNW_SH2_NO_FASTLOOPS`, `GNW_PP_NO_RUNDET`, `GNW_MD_ABLATE`.
- **>4 MiB carts bank now** (`037fe7aa`, picodrive `eeca1e72`): one missing call,
  40 bytes of overlay. The official 5 MiB D32XR release loads for the first time.
- **`RIG_FB_DUMP` + `fbdump_to_png.py`** — read a frozen screen instead of
  guessing. This is what showed the gate had been passing a death screen.
- **GATE3 fixed** — it accepted blank -> still as "moving", which is the shape of
  a fatal error; D32XR sat on `Z_Malloc: failed on 496` through a whole campaign
  of GATE3 PASSes.

## Two traps worth carrying forward

- **md5 both arms before believing an A/B.** Two A/Bs today showed a delta of
  exactly zero because the arms were the same binary — once from a define that
  was already unconditional, once from a handler that already existed earlier in
  the same function.
- **grep before implementing.** A hand-written BF/S countdown collapse turned out
  to sit twenty lines above where it was being added.
