# Sega 32X (picodrive) — read the verdict before you optimise

This core was **removed from the firmware on 2026-07-27** and restored on
`exp/32x-d32xr` for one specific experiment. The performance axis is closed, by
measurement, and the numbers are in **[docs/32X_CLOSED.md](../../../../docs/32X_CLOSED.md)**
with the full session log in
[docs/32X_DEVICE_MEASUREMENT_LOG.md](../../../../docs/32X_DEVICE_MEASUREMENT_LOG.md).

The short version: a heavy frame is 24.4 M device cycles and msh2 is 67.9% of
it. Doubling the frame rate needs 12.2 M cycles removed; zeroing *everything
except msh2* removes 7.3 M. A hand-tuned interpreter floor is ×1.5 and no more,
and a dynarec has nowhere to live — 656 KB of RAM_EMU's 692.5 KB is the emulated
console's own memory.

So do not reopen "make the interpreter faster". What was left deliberately
unmeasured, and is the only reason this branch exists:

- **D32XR (Doom 32X Resurrection)** — a community rewrite of Doom that does far
  less work on the same hardware. Zero code on our side: swap the ROM, compare
  the same scene.
- **2D titles** (Knuckles' Chaotix, Mortal Kombat II, NBA Jam TE) — everything
  ever measured here (Doom, After Burner) is the heaviest class, SH-2 software
  3D. This decides whether the core is unusable or only 3D is.
- **68K poll-skip**, worth at most ~7%.

Measurement rules specific to this core: device dumps cannot be A/B'd across
scenes (`cycles/guest-insn` moved 85.7 ↔ 101.6 with the code unchanged); only
the per-event numbers (fetch 10.3, load 29.3, store 38.5 cycles) are
scene-stable. And `MD32X_DEVICE_PROFILE=1` itself costs ~17% of every
instruction, so a profiled fps is not a shipping fps.
