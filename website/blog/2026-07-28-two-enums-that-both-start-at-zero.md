---
slug: two-enums-that-both-start-at-zero
title: 'Two enums that both start at zero, and a pet that slept walking'
authors: [jshsakura]
tags: [homebrew, fault]
image: /img/clock-hero.jpg
---

The bug report was four words long: the pet sleeps walking sideways.

It was right. The "Zz" appeared over its head, the sleep state was correct, the
status bar said asleep — and the sprite played a walk cycle, striding sideways on
the spot with its eyes shut.

{/* truncate */}

## Two enums

The port has its own behaviour states:

```c
enum { ACT_IDLE = 0, ACT_SLEEP, ACT_EAT, ACT_HURT,
       ACT_POSE, ACT_NOD, ACT_BREATH, ACT_COUNT };
```

The sprite packs have their own action ids, baked into the files:

```c
enum : uint8_t { PMD_IDLE = 0, PMD_WALKL, PMD_WALKR, PMD_SLEEP, PMD_EAT,
                 PMD_HURT, PMD_ATTACK, PMD_POSE, PMD_HOP, PMD_NOD,
                 PMD_BREATH, PMD_SIT, PMD_NACTS };
```

The function that draws the pet indexes the *pack*:

```c
drawPmdAct(act, x, groundY, t, loop, sil, 5);   // act is a PMD_*
```

and the code passed it an `ACT_*`.

```
ACT_SLEEP  = 1  ->  PMD_WALKL    a sleeping pet walks left
ACT_EAT    = 2  ->  PMD_WALKR    feeding plays walk-right
ACT_HURT   = 3  ->  PMD_SLEEP    being sad plays sleep
ACT_POSE   = 4  ->  PMD_EAT      a pose plays eating
ACT_NOD    = 5  ->  PMD_HURT
ACT_BREATH = 6  ->  PMD_ATTACK
```

Every single animation was the wrong one. **Except idle**, because both enums
start at zero.

## Why that is the interesting part

If everything had been wrong, this would have been found in an afternoon. But the
pet is idle most of the time — it stands there, it breathes, the cursor blinks —
and idle was correct. What you saw was a mostly-right character that occasionally
did something odd, which reads as *quirky animation choices*, not as a bug.

Both values were legal. Both were in range. `drawPmdActM()` was handed a valid
action id every time and drew it faithfully. No sanitiser fires, no assert trips,
no test fails, because from the blitter's point of view nothing was wrong.

The type system had nothing to say either: both enums are `uint8_t` underneath, so
the compiler saw an integer going into an integer parameter. C++ would have caught
it if they had been distinct enum classes, which is the real lesson — but the pack
format is upstream's and the behaviour states are ours, so making them different
types after the fact is a change to a file we want to keep diffable against
upstream.

The fix is a mapping function, `pmdActFor()`, plus a gate that says no `ACT_*` may
ever reach `drawPmdAct()` again. And while writing the mapping it became obvious
that the walk cycles — `PMD_WALKL` and `PMD_WALKR`, present in every pack — were
being used by **nothing**. The pet's wander slid it across the ground playing its
idle animation, while its walk cycles went to sleep and eating.

## And then the eating animation still was not there

With the mapping fixed, feeding a berry should play `PMD_EAT`. It did not. The pet
just stood there.

So I counted what the shipped asset container actually contains, across all 302
sprite packs:

```
PMD_IDLE   302/302     PMD_EAT     54/302   <- not in every pack
PMD_WALKL  302/302     PMD_POSE    58/302   <- not in every pack
PMD_WALKR  302/302     PMD_NOD     52/302   <- not in every pack
PMD_SLEEP  302/302     PMD_SIT     52/302   <- not in every pack
PMD_HURT   302/302     PMD_BREATH  50/302   <- not in every pack
PMD_ATTACK 302/302
PMD_HOP    302/302
```

**`PMD_EAT` exists in 54 of 302 packs.** The sprite source simply has no Eat
animation for 82% of the species. "Play `PMD_EAT`" was silently becoming "play
idle" for most of the roster, because the loader falls back to idle for an action
a pack does not carry.

My first instinct was that our own repacker had dropped them — it rescales every
pack to fit a 124 KB slot, so dropping frames would be a plausible thing for it to
do. It does not: it copies every action through with the same id, and only changes
the pixel dimensions. That is now stated in the tool itself, so the next person
chasing a missing animation does not spend the afternoon I did reading a packer
that was innocent.

The real fix is a fallback chain to an action that is actually present:

```c
static uint8_t pickAct(uint8_t primary, uint8_t fallback) {
  if (pmd.has(primary))  return primary;
  if (pmd.has(fallback)) return fallback;
  return PMD_IDLE;
}
...
act = pickAct(PMD_EAT, PMD_HOP);   // a happy bounce, and every pack has one
```

Now every species reacts to being fed, whether or not its artist drew it eating.

## The part that made it survive

The three starter species — the ones you actually raise, the ones I tested with —
have the **full** twelve-action set. Of course they do: they are the most drawn
characters in the source material.

So the developer's pet always had every animation, and the counting only happened
because a user said "there is no eating animation" about a pet I was not raising.
A census of the data would have shown it in one command. I had never run one, and
now the asset verifier prints it on every build.
