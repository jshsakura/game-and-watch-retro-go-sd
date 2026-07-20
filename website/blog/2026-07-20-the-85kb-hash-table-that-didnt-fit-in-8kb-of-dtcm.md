---
slug: the-85kb-hash-table-that-didnt-fit-in-8kb-of-dtcm
title: 'The 85 KB hash table that did not fit in 8 KB of DTCM'
authors: [jshsakura]
tags: [snes, performance, hardware]
image: /img/clock-hero.jpg
---

A game boots, runs for a second, and dies with `need=16392` scribbled in the
log. That number is the whole clue. It is not a fault address, not a PC, not
an LR. It is a `malloc` that returned `NULL` and took the emulator with it.

The game was Super Mario World. The `malloc` was mine. And the data structure
behind it was correct — the bug was *where it lived*.

{/* truncate */}

## The hot path

Super Mario World on this device runs from a static recompiler (`rc`), not the
SNES interpreter. Every opcode, `cpu_runOpcode` calls `rc_dispatch` to ask "is
the current PC a site I translated?" If yes, jump to the translated block; if
no, interpret. That dispatch is the hottest path in the core — it runs once
per guest instruction, so it has to be `O(1)` or the interpreter's 42% speed
win evaporates.

The data structure was a **Knuth multiplicative hash**, open addressing with
linear probing:

```c
slot = (pc * 2654435761u) & mask;   // probe from here
```

Table size `sz = next_pow2(count * 2)`, so load factor stays ≤ 0.5. Most
lookups hit on the first cache line. It is the right structure for the job. I
spent a morning confirming it *was* the right structure — I priced the
alternatives:

| option | memory | lookup cost | verdict |
|---|---|---|---|
| open-addressing hash (load 0.5) | 85 KB | 1 cache line, usually | **keep** |
| binary search, build-time sorted | 33 KB | 11–12 loops in bank 0 | eats the 42% win |
| 2-level page table | 102.5 KB | 2 memory accesses | bigger *and* slower |

The hash won on every axis except one. The one it lost on was the only one
that mattered on the device.

## The heap that ran out

The 8,371 translated sites split across 7 banks. Bank `0x00` alone has 3,767
sites → an 8,192-entry table → 32,768 bytes. Bank `0x04` has 1,501 sites → a
4,096-entry table → 16,384 bytes. Total: **85 KB**, in 7 per-bank tables.

`rc_dispatch_init` allocated each table with `malloc`. On a desktop that is a
non-event. On this device, `malloc` allocates from **DTCM** — the same 81 KB
the launcher lives in. The launcher takes 72 KB of it. That leaves about 8 KB
for everything else.

The first per-bank `malloc` (1,024 entries, bank `0x07`) succeeded. The
second (2,048 entries, bank `0x01`) succeeded. Then bank `0x04` asked for
16,384 bytes plus the 8-byte allocator header, and DTCM said no:
`need=16392`. The SMW core dereferenced the returned `NULL` and the device
died on the first frame.

A desktop harness would have given it 85 KB out of gigabytes and never
noticed. The device noticed on the first boot.

## The fix is one linker decision

Move the tables out of DTCM into `RAM_EMU`. `RAM_EMU` is the 724 KB region
each emulator core gets to itself (see
[the 32X ITCM devlog](/devlog/32x-fighting-for-1740-bytes-of-itcm) for the
shape of that budget). The SNES overlay uses 437 KB of it; 85 KB more leaves
212 KB of margin. The tables are per-core state, they are mutually exclusive
with every other core's overlay, and `RAM_EMU` is exactly where per-core
state belongs.

So `malloc` disappears. The tables become a static `__attribute__((section
(".overlay_snes_bss")))` array. Initialisation becomes a `memset` at boot.
The 8 KB DTCM heap is untouched.

```ld
ASSERT(ABSOLUTE(_OVERLAY_SNES_BSS_END) < __RAM_EMU_END__,
       "Error: SNES BSS overflow");
```

That `ASSERT` is the whole point of the move. Before, the budget was a
runtime OOM — invisible until the device booted, after the build was green,
after the ELF was flashed. After, the budget is a **link error**: add a
translated site that pushes the table past `RAM_EMU`, and the build fails at
`ld`. The fault moved from "the device tells you in a second" to "the linker
tells you before you flash."

## The lesson I actually learned

Two lessons.

The first: **the right data structure in the wrong heap is the wrong data
structure.** I had spent a morning proving the hash beat binary search and
the page table on lookup cost. All three were correct on lookup cost. None of
them were correct in DTCM, because none of them fit in 8 KB, and the one that
fit worst was the one I had shipped. The axis I had not priced was "whose
heap is this." The launcher owns DTCM. The core owns `RAM_EMU`. A core that
mallocs from the launcher's heap has stolen a resource it cannot return.

The second: **a runtime OOM that becomes a link error is the whole win.** The
device's `need=16392` was a debugging clue only because I had a person
reading the log. Users do not read the log. They see a black screen. The
`ASSERT` in the linker script means the build fails in CI, on my machine,
before anyone flashes anything — the budget became a gate, not a hope.

The data structure did not change. The hash is still Knuth multiplicative,
still open-addressing, still load-factor 0.5. It was right about all of that.
It was wrong about *where it lived*, and that was the only thing that
mattered on the device.
