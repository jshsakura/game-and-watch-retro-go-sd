---
slug: the-roms-that-didnt-speed-up-are-the-data
title: '32X: the ROMs that did not speed up are the data'
authors: [jshsakura]
tags: [32x, performance, hardware]
image: /img/clock-hero.jpg
---

You ship an optimisation. Seven ROMs speed up by 28–44%. Eight ROMs do not
speed up at all. The temptation is to declare victory on the seven and move
on. The discipline is to read the eight, because the eight are the data —
they tell you the bottleneck is not the thing you optimised, and the next
optimisation has to look somewhere else.

This is the 32X performance story. It is not a story about a clever fix. It
is a story about how to chase a performance bug without lying to yourself,
and the four profilers you need to do it.

{/* truncate */}

## The lever, and what it did

The SH-2 interpreter in PicoDrive had two shapes of idle loop that together
covered a lot of 32X games:

1. `BRA $; NOP` — an unconditional branch to itself, the SH-2 equivalent of
   `while (1) ;`.
2. One-to-four instruction bodies ending in a backward `BF` (branch if
   false) on a `DT Rn` (decrement-and-test) countdown — the classic
   `for (i = N; i > 0; i--) { ... }` shape.

We wrote a fast-loop detector for both, made it cycle-exact,
timeslice-bounded, IRQ-aware, and gave it a **negative cache** (more on
that below). The results were unambiguous:

| ROMs | Result |
|---|---|
| Chaotix, Zaxxon 3D, Virtua Racing, Doom, Virtua Fighter, Tempo, Metal Head | **+28% to +44%** |
| Kolibri, Stellar Assault, Star Wars Arcade, NBA Jam, WWF Raw, Primal Rage, Pitfall, After Burner | **~0%** |

Fifteen ROMs, seven winners, eight no-matches. The easy reading is "the
optimisation works on the seven." The honest reading is "the eight tell me
the bottleneck is somewhere else, and if I want to speed *them* up I have
to find it."

## Why the negative cache matters

The fast-loop detector has to *try* a candidate loop before it knows
whether it matches. "Try" means speculatively executing the fast path until
either the loop exits normally (match confirmed) or the expected invariants
break (mismatch, roll back). Rolling back is expensive.

The **negative cache** remembers loops that have already been tried and
rejected, so the detector does not pay the probe cost twice. Without it,
Kolibri — which has no matching fast loop — loses ~4–5% to repeated probes
of hot loops that never match. With it, the no-match cost is one probe per
loop, amortised to zero.

This is the trap in "let me just try adding a fast-loop detector for this
shape." Every shape you add has a probe cost on every ROM that does not
match it. The negative cache makes that cost bearable; it does not make it
free. A speculative detector that matches nothing is a slowdown.

## Four profilers, four different lies

The discipline is to measure before you optimise. The trap is that each
profiler proves a different thing, and reading the wrong one for the
question will send you after the wrong bottleneck.

| Tool | What it proves | What it does *not* prove |
|---|---|---|
| QEMU phase profile | ARM Thumb instructions by PicoFrame phase | Device cache, flash/PSRAM wait states, real fps |
| QEMU frame histogram | Typical and tail instruction cost per frame | Cortex-M7 cycle cost |
| SH-2 guest-PC histogram | Which guest PCs / opcodes / loop edges run most | That skipping a loop is timing-safe |
| Device DWT ledger | Real Cortex-M7 cycles, including memory effects | Guest-PC attribution (unless you also wire a compact PC profiler) |

QEMU runs at `-icount shift=0`, calibrated to ~40 Thumb instructions per
CMSDK tick. Its numbers are **deterministic A/B instruction counts**, not
fps. Never convert them to fps. Never claim they equal M7 cycles. They are
a *relative* measure: did the candidate execute fewer instructions than the
baseline, at the same checkpoints?

The guest-PC histogram is the one that finds the next loop. Run it with
fast loops **off** first, so the loops the existing lever removes are
visible. Then run it with fast loops **on**, to expose the residual hot
set — the loops that are still executing because no detector matches them.
The residual set in the no-match ROMs is the data.

The DWT ledger is the one that decides whether a QEMU win survives on
silicon. A 30% QEMU instruction reduction that lands in a memory-wait-bound
phase can be a 0% device speedup. Verify on hardware before you ship.

## The acceptance bar for a new lever

A candidate fast-loop shortcut has to clear all of these before it ships:

- **≥3–5% of guest instructions** in a gameplay window, or comparably
  strong host-phase evidence. Below that is QEMU noise.
- **Appears in more than one window**, ideally more than one ROM. A
  title-only hotspot is a title-only optimisation.
- **The exact instruction sequence and control-flow edge are known.** No
  "this looks like a loop." Disassemble it, document the edges.
- **Memory / IO behaviour is understood.** Reject any loop that reads or
  writes memory-mapped IO, polls communication / timer / VDP / PWM / FIFO
  state, can observe an interrupt mid-loop, or contains side effects that
  are merely inconvenient to model.
- **Repeat with a save-state round trip.** Same ROM fingerprint, same
  frame count, same input script, same compiler flags, same warmup. The
  framebuffer checkpoints must match at every probe point.

If the data says the bottleneck is in the draw, the compositor, the audio,
the scheduler, or memory waits — **stop searching for another SH-2 loop
shortcut.** Instrument that phase next. A 44% win on Doom tells you nothing
about Kolibri, and Kolibri is the ROM that needs the work.

## p95, not average

The frame-cost histogram reports `p50 / p90 / p95 / p99`, plus min/max and
20 fixed bins. **The average is the wrong metric.** A lower average with a
worse p99 can still produce audio underruns and visible frame pacing — the
tail is what the user experiences. Budget for the p95/p99 misses, not the
mean.

At 340 MHz, a 60 Hz frame budget is ~5,666,667 cycles; 50 Hz is
~6,800,000. The histogram's over-budget count is the regression test: it
must go down, or the optimisation did not land on the device.

## The lesson I actually learned

**The ROMs that did not speed up are the data.** A win on seven out of
fifteen is a real win — ship it. But the eight that did not move are the
ones that tell you what to do next, and the next thing is almost never
"write a third loop detector." It is "find out where those eight are
spending their time, because it is not in a loop the existing detector
matches."

The general shape: when a measurement-and-optimise pass produces a mixed
result, the wins tell you the lever works; the no-moves tell you the lever
does not apply, and the residual hot set in the no-moves is the next
investigation. A performance story that reports only the wins is a sales
pitch. The
[32X performance results file](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/perf/32x-histogram/docs/32X_PERFORMANCE_RESULTS.md)
records both, because the no-moves are what the next session needs.

Measure first. Optimise second. Report both. The eight are the data.
