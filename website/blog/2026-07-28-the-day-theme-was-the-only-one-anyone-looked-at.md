---
slug: the-day-theme-was-the-only-one-anyone-looked-at
title: 'The day theme was the only one anyone ever looked at'
authors: [jshsakura]
tags: [homebrew, testing]
image: /img/clock-hero.jpg
---

A photo arrived of the status card at night: a blank white slab in the middle of
the screen, and a blank white pill below it. Both were supposed to have text in
them. Both were drawing it perfectly.

{/* truncate */}

## One line

A widget's surface is a fixed colour — a white pill, a beige well. Its label was
drawn in `inkColor()`:

```c
static uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }
```

In the day theme the ink is dark, so dark text landed on a light tile and
everything read correctly. At night the ink turns **light**, and light text landed
on the same light tile.

Every screen in the port had that exposure. The status card was simply the one
with the largest light surface, so it was the one that got photographed.

## The fix is not a colour, it is a source

The wrong instinct is to special-case the card. The right one is to notice that
the ink was being asked of the *theme* when it is a property of the *surface*:

```c
/* Ink that can be READ on `surface` -- derived from the surface, never from the
 * theme. */
static uint16_t inkOn(uint16_t surface) {
  const uint32_t r = ((surface >> 11) & 0x1F) << 3;
  const uint32_t g = ((surface >> 5)  & 0x3F) << 2;
  const uint32_t b =  (surface        & 0x1F) << 3;
  const uint32_t luma = (r*30 + g*59 + b*11) / 100;
  return luma > 140 ? UI_INK : UI_INK_NIGHT;
}
```

Luma, not a lookup table, so it answers for any surface anyone adds later —
including one blended at runtime. Every tile now returns the ink its caller must
write with, and a "well" (a recessed readout) is *darker* than its page rather than
a fixed beige that is brighter than the night background it sits inside.

The same class of bug had been found earlier the same day at the other end of the
screen: the bottom stat panel was an opaque `#383844` slab where the original game
has translucent white, with its labels in `inkColor()` — dark grey on dark navy.
That one was reported as "the panel colour is wrong". It was also unreadable.

## The gate, and the two ways it was wrong first

How do you test "is this readable"? A contrast metric against the theme's
background is worthless — the surface is not the background. Comparing renders is
worthless — the pet wanders, the cursor moves.

What works is **flatness**. A widget that contains a label has a luma spread inside
its own rectangle. One that has swallowed its label is uniform. That is mechanical,
needs no aesthetic judgement, and fails on exactly the thing that was broken.

The focus sets already describe every pressable rectangle, so the check gets its
geometry for free. Against the old ink it names seven findings.

Two mistakes on the way there, both instructive:

**It only tested the selected state.** The first version pointed the cursor at each
widget in turn and measured it. But the selected look is an accent-coloured surface
that always contrasts — so the check sailed past the very tile in the photograph.
It has to measure both states.

**Nothing had ever rendered the night theme.** The harness produced 24 screen
captures: twelve screens in two languages, all in daylight. Not one image of the
theme that was broken had ever existed. It now renders 48 — both themes — and the
first thing that fell out was a white slab exactly where the photo showed one.

## The lesson I keep relearning

The harness was not missing a *check*. It was missing a *state*. Every check it had
would have caught this instantly if it had ever been pointed at the night theme,
and none of them could catch it while the tree only ever rendered daylight.

When something ships broken in a mode nobody tests, adding a smarter test for the
mode you do test will not find it. Render the other mode.
