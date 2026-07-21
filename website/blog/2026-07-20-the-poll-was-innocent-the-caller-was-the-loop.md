---
slug: the-poll-was-innocent-the-caller-was-the-loop
title: 'GBA idle skip: the poll was innocent, the caller was the loop'
authors: [jshsakura]
tags: [gba, performance, audio]
image: /img/clock-hero.jpg
---

A Pokémon intro takes six frames on hardware. On the device it took seven
hundred. Same ROM, same emulator, same idle-skip table — and the idle-skip
table was the thing that was wrong, even though every entry in it was
correct.

This is the story of the second idle-skip semantic. The first one was right
about loops; the second one had to exist because some games do not loop on a
poll, they *call* one.

{/* truncate */}

## The first semantic: spin until an interrupt

Most GBA games idle the CPU by spinning on a register until an interrupt
releases them. The shape is:

```asm
wait:
    ldr  r0, [VCOUNT]      ; read the scanline counter
    cmp  r0, #160
    bne  wait              ; not yet, spin
```

The CPU burns the loop, the interrupt fires, the loop exits, the game does
one frame of real work and comes back. We do not need to emulate the spin.
We detect the loop's PC, and when the guest reaches it, we skip the slice —
burn the guest forward to the next interrupt. Same visible behaviour, none
of the cost.

That is the **classic table**: `idle_loop_target_pc` plus
`idle_loop_cond == ALWAYS`. Reach this PC, always skip. It is right for
every loop that only an interrupt releases. We generate it per-cart in a
sister repo and ship the table baked into the core.

It is right. It is also incomplete.

## The game that broke it

One cart — call it the slow one — paced its intro differently. It wanted to
wait for scanline 160, but instead of spinning inline it **called** a
`wait_for_scanline_160` subroutine a *counted* number of times:

```asm
    mov  r4, #120          ; pace the intro with 120 waits
pace_loop:
    bl   wait_for_scanline_160
    subs r4, r4, #1
    bne  pace_loop
```

On real hardware, ~120 of those calls return instantly, because the
subroutine is reached while the scanline is already past 160 — the inner
poll exits on the first read. The intro takes six frames.

Our `ALWAYS` skip did not know about "counted calls." Every time the guest
hit `wait_for_scanline_160`'s top — the PC we had correctly identified as a
poll — we skipped the slice. But the slice we skipped was a single iteration
of the *outer* loop, the one that called 120 times. So each call consumed a
full slice of guest time instead of returning instantly. Six frames of
intro became seven hundred. The game looked frozen. The intro music played
three notes and stopped.

The idle-skip detector had done its job. It found the poll. It skipped the
poll. The poll was innocent. The caller was the loop.

## The second semantic: skip while it loops

The fix was not "delete the entry." The fix was a second condition:
`idle_loop_cond == WHEN_NE`.

Park on the poll's **closing branch** — the `bne wait` at the bottom — and
burn the slice only *while Z says it loops*. The branch is the only place
the loop's state is observable. If Z is set, the loop is exiting this
iteration; do not skip, run the caller forward. If Z is clear, the loop is
spinning; skip.

```c
if (idle_loop_cond == WHEN_NE && !(cpsr & FLAG_Z))
    skip_slice();
```

That single-bit read is the whole difference between "this PC is a place we
idle" (ALWAYS) and "this branch is a place we *would* idle if we were
spinning" (WHEN_NE). The counted-call loop now runs the outer loop's 120
iterations at hardware speed, because each call's closing branch has Z set
on the first poll — "the scanline is already past 160, exit" — and we do
not skip.

The two semantics share one target slot per cart. Only carts with no
generated entry need a hand-curated `vcount_polls[]` row in `main_gba.c`.

## The proof bar

An idle-skip change is the kind of thing that can speed a game up by
silently breaking it. The proof bar is a 1,800-frame no-keys A/B against
the unmodified core:

- Both runs render every frame to a hash.
- The two hash streams must be ~99.8% identical, at a small fixed shift
  (the skip changes exactly when the interrupt is taken, which moves every
  subsequent frame by a constant).
- The two streams must be **mutually contained** — neither has frames the
  other does not. A dropped or duplicated frame is a behaviour regression,
  not a speedup.

The WHEN_NE change passed. The slow cart's intro dropped from 700 frames
back to 6. Every other cart in the table was unchanged.

## The traps that came with it

The same pass that added WHEN_NE surfaced three more GBA footguns, each its
own small story:

- **PSG note-ons a fourth flat.** The PSG engine had `65536` typed in two
  files — one as a formula, one as a literal — and they had drifted. Notes
  were a fourth off because the wrong one was being read. One definition
  now, pinned with a `_Static_assert` so the next drift is a build error.
- **`tanf()` overflowing resident flash by 1,412 bytes.** The audio filter
  used `libm`'s `tanf`, which is large, and resident flash is shared with
  the launcher. A single `tanf` pushed the build over. The filter now uses
  a Padé rational approximation — same response, a few hundred bytes of
  code instead of kilobytes of library.
- **The census that credited six games a mixer they never ran.** The M4A
  census had tagged six carts as "uses the M4A HLE mixer." They did not.
  They had the mixer code copied into IWRAM as dead bytes — present,
  measurable, never executed. Adoption now requires **burned cycles**: a
  cart is only credited if the mixer actually runs, not if it merely exists
  in RAM. Our runtime hook is safe either way (a PC never reached never
  fires), but the census is what tells you which carts to optimise for.

## The lesson I actually learned

**A detector that cannot tell spin from call will mis-fire on the call.**
The first idle-skip semantic was correct about loops. It was wrong about
subroutines, because a subroutine's top-PC looks identical to a spin loop's
top-PC — the difference is one branch outcome one frame up, and the
detector was not looking there.

The general shape: when a hot-path optimisation mis-fires, the bug is
rarely in the optimisation. It is in the *distinction* the optimisation did
not make. ALWAYS skipped every poll at that PC; it needed to also ask "is
the thing that reached this PC spinning, or calling?" The fix was one bit
of CPU state read at one branch. The hard part was noticing the question
existed.

The
[GBA full-speed devlog](/devlog/gba-pokemon-full-speed) covers the M4A HLE
work that made the audio path keep up. This entry is the one that came
after — the one where the speedup itself mis-fired, and the fix was to
teach the detector a second verb.
