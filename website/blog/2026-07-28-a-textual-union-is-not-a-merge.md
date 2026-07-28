---
slug: a-textual-union-is-not-a-merge
title: 'A textual union is not a merge: taking upstream v1.4.0 back'
authors: [jshsakura]
tags: [upstream, gba]
image: /img/clock-hero.jpg
---

Upstream released v1.4.0 with nine commits, and most of them were this fork's own
GBA work coming home — `gba_redefines`, the M4A mixer HLE, even the porting
playbook are byte-identical upstream now. Merging that back should have been the
easy direction.

It produced two `emu_gba` definitions, two `GBA_CODE` memory regions, five doubled
externs, and a linker script that could not link.

{/* truncate */}

## Why the easy direction was not easy

Git merges by common ancestor. The GBA core does not have one: upstream added it,
this fork added it, and the two additions met for the first time in this merge. To
git that is *add/add* on every line of every GBA file — and for identical or
near-identical content it does the reasonable thing and keeps both sides.

Both sides being the same feature, the result is the feature twice:

```
STM32H7B0VBTx_SDCARD.ld     GBA_CODE region declared twice
rg_emulators.c              two `static const emu_dispatch_t emu_gba = {...}`
                            and two dispatch branches for the same system name
gw_linker.h                 five externs, each declared twice
```

Duplicate `extern` declarations are legal C, so the header compiled and said
nothing. The duplicate `emu_gba` is a duplicate symbol; the duplicate memory region
is a linker script that means something different from what either author wrote.
None of it is a *conflict* in git's sense, so none of it appeared in the conflict
list. It appeared when the build fell over.

## The method that worked

For any file where both sides added the same feature, do not read the merge output.
Compare the two inputs by **symbol set**:

```sh
git show upstream/main:$F | grep -oE '<symbol pattern>' | sort -u > /tmp/theirs
git show HEAD:$F          | grep -oE '<symbol pattern>' | sort -u > /tmp/ours
diff /tmp/theirs /tmp/ours && git checkout HEAD -- $F
```

If ours is a superset, take ours whole and move on. That is a two-line proof that
nothing of theirs is lost, and it is far more reliable than reading a 200-line
merge hunk and deciding it looks right.

## Where upstream wins, and where it cannot

The house rule for this fork is that upstream is the reference: their release is
merged as it lands, and when a merge conflicts, **their version is the one taken**.
That is not deference for its own sake — it keeps the fork mergeable in the other
direction, which is how the GBA port got upstreamed in the first place.

Five conflicts could not be resolved that way, and each is documented at the line
in the source rather than in a commit message nobody will find:

**`appid.h`.** Upstream numbers `LYNX = 20, GBA = 21`. This tree has NGP, Lynx,
Virtual Boy, Super Metroid, GBA, SNES and 32X at 20…26, because it has those cores.
Taking upstream's numbering shrinks `APPID_COUNT` from 27 to 22 — and that enum
sizes an array inside the persisted config struct, so the saved config stops
matching its own magic and **every user's language, coverflow, backlight and volume
reset to defaults**. A merge cannot be allowed to do that.

**The logo index.** `/bios/logo.bin` is indexed positionally, and the only thing
backing that index is the link order of some structs. Ours already has the GBA
header logo in the slot the shipped file needs; upstream appends its own at its own
tail. Taking both duplicates a symbol and **shifts every index behind it on cards
that are already out in the field**.

**The overclock call.** Upstream calls `SystemClock_Config(3)` directly for GBA.
This fork has `common_emu_auto_oc()`, which upstream does not — and it is the only
thing that skips the boost entirely on one SD-hardware variant that crashes when
overclocked, and that honours a user who chose a higher level. The clock is the
same either way; the door has a guard on it. (This one was later taken verbatim
anyway, by the maintainer's decision, with both consequences written into the file
so they are a choice and not a surprise.)

**Three files where upstream's version is this fork's own code with shorter
comments** — the flash allocator's live set, the frame-pacing integrator, the
emulator table. Identical logic, including the same constants. Ours keeps the
paragraphs that explain where the numbers came from.

## What the merge found on its own

Merges are a good time to read code you have not read in a while. Two things fell
out that had nothing to do with upstream:

The extflash packaging recipe had **two `$(BIN)` lines**, the second silently
overwriting the first's output with a different section list — one that omitted two
cores and contained `".overlay_a2600-j .overlay_lynx"`, a missing space that makes
one bogus section name and drops Lynx entirely. That is what had been shipping.

And the build's per-core object directories are made by a rule with an order-only
prerequisite, which is satisfied by mere *existence*. So any tool that creates
`build/` first — the layout harness makes `build/tamapoke_harness` — means the rule
never runs and none of the 36 subdirectories exist. The next build dies on
`can't create build/gba/gba_bios.o`: a file in a core nobody touched.

Neither of those was upstream's doing. Both were found because a merge makes you
open files you would otherwise trust.
