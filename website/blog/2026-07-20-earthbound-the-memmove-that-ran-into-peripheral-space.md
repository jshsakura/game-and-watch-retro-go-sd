---
slug: earthbound-the-memmove-that-ran-into-peripheral-space
title: 'EarthBound: the memmove that ran off into peripheral space'
authors: [jshsakura]
tags: [snes, fault, hardware]
image: /img/clock-hero.jpg
---

A game freezes on the device. The same game runs clean on the host for thousands
of frames. You have a backtrace. The backtrace points at `memmove`. You stare at
it, because nobody writes a bug into `memmove`.

{/* truncate */}

This is the EarthBound story — or rather, the story of a bug that was not in
EarthBound, and not in `memmove`, but in the thing that stood between them: the
compiler's register allocator, deciding which argument went where.

## The symptom

EarthBound's `scroll_window_up` — a routine that moves the text window's tilemap
up by one line when a dialogue box scrolls — ran fine in the host harness for
four thousand frames. On the device it took an **imprecise bus fault** partway
through a scene. The screen froze. The BSOD gave the usual: a title, a PC, an
LR, nothing else. (We never wire up `SCB->SHCSR`, so every fault escalates to a
bare "Hardfault" — you can read about that choice in the
[boot rescue devlog](/devlog/boot-rescue-when-a-hung-boot-was-a-dead-battery).)

The PC landed in `memmove`. The LR pointed back at `scroll_window_up`. The
obvious read: we passed `memmove` a bad pointer, it dereferenced it, we faulted.

Except the pointer was fine. The struct it came from was fine. The size was
fine. Everything in the source was correct, and the host — the *same source,
same data, same struct layout* — never faulted.

## The thing that was different

The host is x86-64. The device is a Cortex-M7. And the M7 has one rule x86 does
not share, that I had been pretending did not matter: it traps on **unaligned
64-bit accesses**. A `STRD` (store two words) to an address that is not
word-aligned faults. Halfword and word accesses are fine — unaligned is legal
for those — so the SNES code, which does a thousand unaligned halfword stores a
frame, runs happily. Only the 64-bit ones trap.

But `memmove` does not do a 64-bit store. We don't write `*(uint64_t*)` anywhere
in `scroll_window_up`. We call the library.

So I disassembled `memmove`. And there it was — not a 64-bit store, but
something worse.

## The codegen bug

`scroll_window_up` had been compiled with `arm-none-eabi-gcc 15.2`. The function
was hot/cold-split (`.part.0`) because it had a rarely-taken path. The arguments
to `memmove` — dest, src, size — were passed in registers `r0`, `r1`, `r2`.

Except they were not.

The dest and src both came from a struct member typed with `ABI_PTR_ALIGN`
(`__attribute__((aligned(8)))`). Inside the `.part.0` cold split, the compiler
had decided to honour that alignment obligation by … **shifting every argument
one register over**. So `r0` got a pointer, `r1` got the *other* pointer that
should have been in `r0`, `r2` got the pointer that should have been in `r1`,
and `r3` — the size argument — got `nbytes`. `memmove`'s prototype reads its
size from `r2`. `r2` held a pointer. A pointer is a very large number.

The copy size was, in human terms, several hundred megabytes. The destination
was a tilemap in DTCM. The source was valid. So `memmove` dutifully walked
forward, overwriting everything in its path, until it crossed from DTCM into
the AXIM bus and then into peripheral space at `0x40000000–0x5FFFFFFF`, where
the first unmapped slave took the store as an imprecise bus fault.

The PC pointed at `memmove` because that is where the bus drained the buffered
store — not where the *decision* to copy 300 MB was made. The decision was
made in `scroll_window_up`, one stack frame up, by a register allocator that
had silently mis-counted its arguments.

## The fix

Two lines.

```c
uint16_t *dst = w->content_tilemap;
uint16_t *src = /* ... */;
size_t nbytes = /* ... */;
memmove(dst, src, nbytes);
```

Materialise the pointer and the size into plain locals — locals with no
alignment obligation, no `ABI_PTR_ALIGN`, nothing for the allocator to
"honour" — and pass those. The allocator puts them in the right registers.
`memmove` reads the right size. The copy is the 240 bytes you asked for.

That's the whole fix. The compiler stopped getting confused the moment we
stopped handing it an over-aligned struct-member pointer directly.

## The lesson I actually learned

Two lessons.

The first: **if you suspect a codegen bug, read the disassembly.** Not the
source. The source is correct. The bug is between the source and the binary,
and only the binary tells the truth. I spent a day adding asserts to the source
before I disassembled `memmove` and saw `r2` holding a pointer. Five minutes
with `arm-none-eabi-objdump` would have found it on the first morning.

The second: **the host catches alignment faults that do not matter, and misses
the ones that do.** Our `tools/sm_harness/device_run.sh` runs with
`-fsanitize=alignment` *only on 64-bit accesses* — exactly what an M7 traps —
because x86 and aarch64 do not trap unaligned halfword/word stores either, so
flagging them would be noise. But the memmove bug was not an alignment fault.
It was a register-allocation fault that *happened* to be triggered by an
alignment annotation. No host sanitizer catches "the compiler put your argument
in the wrong register." Only the device catches that. And the device does not
tell you why — only *where*.

We added a `_Static_assert` next to the type-punned stores in the SNES code, so
that an alignment assumption the code makes is one the compiler has to prove
rather than one the struct layout grants by luck. But for the register bug
itself, the only defence is the one we already had: a device build, a real
fault, and a person willing to read the disassembly before they trust the
source.

Three Super Metroid releases shipped that could not boot while the host reported
four thousand clean frames
([the earlier devlog](/devlog/super-metroid-three-releases-that-couldnt-boot)). This
was the fourth story in that family — the one that taught us the host does not
trap what ARM traps, and that the bug is rarely in the function the backtrace
names.
