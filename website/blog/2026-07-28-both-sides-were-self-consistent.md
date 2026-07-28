---
slug: both-sides-were-self-consistent
title: 'Both sides were self-consistent, and both were wrong about each other'
authors: [jshsakura]
tags: [homebrew, testing]
image: /img/clock-hero.jpg
---

Every one of the 151 Pokédex thumbnails in this port was drawn wrong, in every
release, for months. Not subtly wrong — wrong palette, wrong offsets, reading past
the end of each record into the next species.

The converter that wrote the file was correct. The firmware that read it was
self-consistent. Nobody had written down what the file actually is.

{/* truncate */}

## The symptom

"The Pokédex images are corrupted." A gallery of coloured noise where the sprites
should be.

## The file

The gallery thumbnails live in one blob, `thumbs.bin`, inside the asset container
the game reads from the SD card. The firmware read each entry like this:

```c
/* 24x24 raw bitmap, scaled by s. Each byte is a palette index into the
 * sprite palette; for thumbnails we just plot a solid cell. */
for (uint8_t r = 0; r < 24; r++)
  for (uint8_t c = 0; c < 24; c++) {
    uint8_t px = thumb[r * 24 + c];
    if (px == 0) continue;
    gfx->fillRect(x + c*s, y + r*s, s, s, spriteColor((char)px));
  }
```

Confident, commented, and wrong in four separate ways. The record is actually:

```
u8  w
u8  h
u8  palCount
u16 pal[palCount]     // RGB565, little endian
u8  px[w * h]         // palette index; 0xFF is transparent
```

So the code drew the `w`/`h`/palette **header** as if those bytes were pixels, used
a fixed 24×24 where the real sizes vary per species (14×24 up to 17×24), ran about
240 bytes past the end of every record into the next Pokémon's data, and looked
each byte up in `spriteColor()` — the palette for the *ASCII* sprite maps, a
completely different asset type.

## How I found the layout

By taking a shipped container apart with a hex dump.

```
entry 0 header bytes:  10 14 07 00 00 a4 5d 6d ...
                       ^^ ^^ ^^
                       16 20  7   ->  w=16, h=20, palCount=7
record length: 3 + 7*2 + 16*20 = 337
observed distance to the next entry: 337
```

That arithmetic matching exactly, on every one of the 151 entries, is the proof.
Decoding a few with those rules produced recognisable creatures.

The layout was documented nowhere: not in the firmware, not in the converter, not
in a spec. The converter delegates the actual packing to an upstream script and
the firmware had an assumption written as a comment. Neither side was lying — each
was internally consistent — and they were only wrong **about each other**.

## Why no test caught it

This is the part worth taking away.

The port has a host harness that renders every screen, checks for drawing outside
the panel, checks the focus highlight is visible, presses every button, and
verifies no screen inherits the previous screen's pixels. It reported green on the
gallery, every run, for months.

**It does not load the sprite packs.** The assets are licensed material that never
enters the source tree, so the harness runs without them — and the gallery falls
back to the flash sprites, a completely different code path. The harness was
rendering a Pokédex that never touched the broken decoder.

That is not a flaw anyone put there on purpose. It is what happens when the thing
you cannot ship is also the thing you cannot test with, and it is worth naming
because the mitigation is not "add the assets to CI".

## What actually closes it

A verifier that reads the produced container **the way the consumer reads it**:

```
python3 tools/tamapoke/verify_assets_dat.py <container>
```

The parsers in it are transcribed from the firmware's own loaders —
`PmdMon::load()`, `parse_actions()`, `SdThumbs::get()` — not from the packers. The
staging script runs it and refuses to stage a card that fails. It catches:

- a record whose length disagrees with the next entry's offset (**the exact shape
  of the shipped bug**)
- a truncated record
- a sprite pack larger than the firmware's 124 KB slot, which cannot load at all
- a pack with no `PMD_IDLE`, the loader's last-resort fallback, so nothing can be
  drawn
- an action id past the end of the action table, which makes the loader reject the
  whole pack and silently costs that species all of its sprites
- an index that does not account for the payload

And it prints an action census, which turned out to answer a completely different
open question the same week.

Its own test builds a container broken in one specific way per case and requires a
finding for each — because a verifier that has never rejected anything is exactly
the comfort the other two had.

## The punchline

Run against the 31.9 MB container that users actually download: **it passes.**

The file was right all along. Every byte of it. The firmware was reading a
different file than the one on the card, and the fix was entirely on our side —
no regeneration, no re-upload, nothing for anyone to download again.

A converter that never reads its own output the way its consumer does cannot see
that. Neither can a consumer that never checks its input against a spec. The gap
is not in either program; it is in the space between them, and something has to
stand in that space on purpose.
