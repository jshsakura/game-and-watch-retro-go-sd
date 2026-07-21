---
slug: 32x-fighting-for-1740-bytes-of-itcm
title: "Fighting for 1,740 bytes — how the 32X SH-2 interpreter finally got into ITCM"
authors: [jshsakura]
tags: [32x, performance, hardware]
image: /img/clock-hero.jpg
---

The Mega Drive 32X port is the one where every optimization is a fight over a
few hundred bytes. The core is one of the heaviest in the tree — Mega Drive at
681 KB of the 724 KB per-core RAM pool is already 94% full before 32X even
starts — and the path to playable Doom 32X runs through a single, very small,
very contested memory region. This entry is the short version of how 1,740
bytes became the difference between the SH-2 interpreter running at zero wait
states or running at all.

{/* truncate */}

## The memory that is faster than cache

The STM32H7 has a 64 KB block of **Instruction Tightly-Cououpled Memory**
(ITCM) — a sliver of RAM that the core can fetch from with **zero wait states**,
independent of the cache system. For an emulator interpreter — code that runs
literally every guest instruction — there is no better place to live. The
existing port already had `itc_malloc()` to claim parts of it for hot routines.

The SH-2 interpreter in PicoDrive is exactly such a hot loop. Under the
`GNW_SH2_FASTLOOPS` flag it uses **computed goto** for dispatch:
`goto *gnw_dt[opcode >> 12];` — a table of label addresses, jumped to directly,
with no branch predictor warmup, no function-call overhead. On a 32-bit
interpreter that runs thousands of times per frame, the difference between this
and a `switch` is the difference between playable and not.

So the goal was simple: put the whole 6,572-byte SH-2 interpreter into ITCM.

## Why "split it" is not an answer

The first thing you reach for when a function does not fit is to **split it**:
pull the cold path — the rare opcodes, the fallback cases — out into a separate
function, leave only the hot dispatch in ITCM. That is the standard move and it
usually works.

It does not work here. **C requires every target of a computed goto to live in
the same function as the goto.** The moment you pull a label out into another
function, the dispatch stops being a computed goto and becomes a `call`/`ret`
pair — pipeline flush, function-call overhead, *per instruction*. That is
exactly the cost ITCM was supposed to eliminate. You would have built the fast
path and then made it slow again to fit it into the fast memory.

So the interpreter has to go in **as one 6,572-byte block**. No splitting. No
evicting the cold path to XIP. The whole thing, or none of it.

## The 1,740-byte gap

ITCM at that point had **4,832 bytes free**. The interpreter needs 6,572. The
gap is **1,740 bytes** — less than half a KiB. So the question stopped being
"how do I shrink the interpreter" and became "what else is sitting in ITCM that
does not need to be there".

The candidates were ranked by how often they are touched, because ITCM only
matters for code or data that is on a hot path. Cold data in ITCM is wasted
data:

| Symbol | Size | Verdict | Why |
| --- | --- | --- | --- |
| `PsndBuffer` | 8,664 B | **move it** | The main sound mixing buffer. Audio mixing is per-frame, per-buffer — not per-instruction. A cache miss here is invisible. |
| `.bss.ym2612` | 3,216 B | **move it** | The YM2612 FM synth state. 32X Doom leans on the SH-2's PWM audio, so the FM chip is cold. |
| `bus map` | 3,840 B | **leave it** | The memory map is touched on every guest memory access. Evicting it trades ITCM for D-cache misses on the hottest path. |
| `HighLnSpr` | 7,680 B | **leave it** | Per-scanline sprite state — written every visible line. Same problem, bigger. |

Moving `PsndBuffer` alone — 8,664 bytes — frees more than the 1,740 needed. The
interpreter fits with room to spare. The cost is one `__attribute__((section
(".ram_uc")))` on a single buffer declaration, and the mixing loop runs from
`RAM_EMU` instead. Nobody hears the difference.

## What I actually learned

The instinct on a memory-budget problem is to make the thing you are trying to
fit **smaller**. That instinct was wrong here, and would have been expensive:
the only way to shrink this interpreter is to break its dispatch, and breaking
its dispatch is the one thing you must not do.

The move that worked was the opposite — **make the thing next to it bigger, by
kicking something else out**. The 1,740 bytes were not hiding in the
interpreter. They were hiding in a sound buffer that had been placed in ITCM
"because ITCM is fast" and then never re-examined. Fast memory is only fast for
the things that need to be fast. For everything else it is just small.

The honest framing for the next 32X lever is the same: the SH-2 interpreter in
ITCM fixes I-cache thrashing, and the next stall is somewhere else — likely the
PWM timer yield overhead that pounds Doom's interrupt rate, or the SDRAM ROM
fast-path. Both are real. Neither is in the interpreter.
