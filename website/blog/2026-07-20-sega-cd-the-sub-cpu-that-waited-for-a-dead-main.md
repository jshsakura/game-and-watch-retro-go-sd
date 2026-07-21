---
slug: sega-cd-the-sub-cpu-that-waited-for-a-dead-main
title: 'Sega CD: the sub-CPU that waited for a main CPU that was already dead'
authors: [jshsakura]
tags: [segacd, fault, hardware]
image: /img/clock-hero.jpg
---

The Sega CD is two computers. A main 68000 (the Mega Drive's) and a sub-68000
(on the CD unit), sharing memory through a gate array, running out of two
different BIOSes, synchronising through a register one writes and the other
polls. We had both running. The disc spun. The BIOS loaded. Then everything
stopped, and the sub-CPU sat at address `$6132` forever.

{/* truncate */}

This is the story of how the Sega CD port spent a week looking like a
sub-CPU bug, and was actually a main-CPU crash the sub-CPU was waiting on.

## The architecture, in one paragraph

The Mega CD adds a second 68000 ("the sub") alongside the Mega Drive's main
68K. The sub runs the CD BIOS and the CDC (CD controller). The two share
**PRG-RAM** (512 KB of sub-CPU work RAM) and **Word-RAM** (256 KB that the
gate array can flip between "2M" mode owned by the sub and "1M" mode split
between both). They talk through a handful of registers at `$A12000`: one
writes a bit, the other reads it, and the handshake is *the* IPC primitive.

Our port runs both 68Ks on **one** Musashi core — gwenesis's device-tuned
68K, with the sub kept as a second `m68ki_cpu_core` struct that we
`memcpy` in and out. That is the PicoDrive model. It works because Musashi
keeps its entire state, including the memory map, inside one struct.

## The deadlock

We booted Sonic CD. The sub-CPU's BIOS initialised. The main CPU's HLE
fast-boot (we skip the long BIOS check and inject the disc's Initial Program
directly) jumped the main CPU to the game's entry point. The sub reached
address `$6132` and stopped. The main CPU reached `$004BB4` and stopped. The
frame was frozen. The audio was silent.

`$6132` is in the sub BIOS. It is an **IPC spin loop**: the sub is waiting for
the main CPU to clear the first `0x97EB` bytes of PRG-RAM (the sub's work
RAM) as part of the boot handshake. The sub spins, reading a status bit,
until the main writes "done." The main never wrote "done." The sub sat there
for the entire run.

Why didn't the main clear PRG-RAM? Because **the main CPU was dead**.

## The main CPU was not slow. It was dead.

The HLE fast-boot had jumped the main CPU to address `$FF0000`. On a Sega CD
disc, `$FF0000` is the start of the **IP header** — the first thing the BIOS
reads off the disc. The first four bytes of that header are the ASCII string
`SEGA` (`53 45 47 41`), which the BIOS validates as a magic number. It is
data. It is not code.

We jumped the main CPU into the middle of the ASCII string "SEGA" and told it
to execute those bytes as 68000 instructions. `53 45` decodes as something
legal-ish; the CPU runs forward through the header data, interpreting
release-year strings and checksum bytes as opcodes, until it hits a byte
sequence that is not a legal instruction. The 68K raises an **Illegal
Instruction exception**. The exception handler vectors through `$210`, which
in the Mega Drive BIOS is an **infinite loop** — the "the cart is broken,
halt forever" handler. The main CPU entered the loop and never came back.

So the sub-CPU's `$6132` spin was not the bug. It was the *symptom*. The
main CPU had crashed ten milliseconds earlier, at the moment of our HLE
jump, because we pointed it at a header instead of at code. The sub was
waiting for a handshake partner that had died before the handshake started.

The fix was one address. The IP entry point is `$FF0100` — past the header,
at the first actual instruction — not `$FF0000`. Change the jump target and
the main CPU runs the game. The handshake completes. The sub leaves `$6132`.
The disc boots.

## The thing that made it hard to see

The sub-CPU's spin address (`$6132`) was in every trace. The main CPU's
crash address (`$210`) was in every trace too, but it looked like noise —
the sort of thing a debugger shows you when a thread is idle. `$210` is an
exception vector address; unless you know the Mega Drive BIOS uses it as a
halt loop, it reads as "the CPU is parked somewhere benign." It is not
benign. It is the coffin.

The trace also showed the sub's interrupt mask as `IEN = 0x54` — Level 5
disabled. The sub BIOS uses the CDC's `DECI` interrupt (Level 5) to know
when a sector has been decoded and is ready to DMA. With Level 5 masked,
`DECI` cannot fire, so the DMA-trigger code never runs, so even if the main
had lived, the sub would still have been stuck one layer down. The mask is
cleared by the main CPU's boot progress — which never happened.

Three things wrong, all downstream of one wrong address.

## The other shape of bug: timing

Once the main CPU lived long enough to handshake, the next set of bugs were
**timing**. The Sega CD's CDC decoder runs at **75 Hz** — the CD-DA sample
rate, derived from the disc spin. Our first port ticked the CDD (the CD
drive controller) once per video frame, which is **60 Hz** (NTSC) or 50 Hz
(PAL). That is a 20% speed error on every CDC state machine. Discs that
relied on sectors arriving at 75 Hz would lose sync, time out, and retry
forever.

The fix: drive the CDD tick from a **true 75 Hz clock**, not from the video
frame. The video frame paces rendering; the CD paces data; they are separate
domains in real hardware, and they have to be separate in the emulator too.

The second timing bug was in the **CDD serial command encoding**. The CDD
talks to the sub-CPU over a serial link, and status responses are encoded in
**BCD** — binary-coded decimal, one digit per nibble. Our first encoder
**packed two digits into one byte and then duplicated them**, so the sub
read each digit twice. Some BIOS versions tolerate this; the Sonic CD sub
BIOS does not. It reads the duplicated digits as a protocol error and rejects
the status. The disc reports "ready" forever.

Fix: **one digit per byte, not packed-and-duplicated.** Two lines.

## The RAM fight (the other half of the port)

The Sega CD is the heaviest core we have ever tried to fit. Simultaneously
resident, it needs about **976 KB of writable RAM** — the CD unit's 840 KB
(PRG-RAM 512, Word-RAM 256, PCM 64, BRAM 8) plus the base Mega Drive's 136
KB. `RAM_EMU` on this device is **724 KB**. We are 250 KB short before we
start.

The only way it fits is the same trick Super Metroid uses
([the XIP story](/devlog/32x-fighting-for-1740-bytes-of-itcm) is the small
version): **eXecute-In-Place all the emulator code from external flash.**
The MD overlay's 584 KB of code plus the CD layer's code go into a
`SEGACD_CODE` linker region at a sentinel address (`0xDEAD0000`-style), and
`store_file_in_flash_relocate()` patches the relocations at load time. The
code never lives in RAM. `RAM_EMU` is then free for data — framebuffers, the
two 68K states, every CD RAM bank.

And then the **scorched-earth** part: a single framebuffer, not the double
buffer we use for every other core (saves 150 KB). Recruit **every CD RAM
bank** as emulator working memory — PRG-RAM and Word-RAM into AXI SRAM, PCM
into AHB, BRAM wherever it fits. Margin is approximately zero. RAM-cart
games (the ones that need a 128 KB backup cart) are out of scope — they
overflow, full stop, and no amount of XIP fixes that.

## What I actually learned

The Sega CD port taught me two things I keep relearning.

**One: when a handshake stalls, the bug is upstream of the handshake.** The
sub-CPU sat at `$6132` because the main CPU died at `$FF0000`. Every trace
showed the sub's stall. No trace flagged the main's crash — it was an
exception vector that *looked* like idle. When you see a spin, walk backwards
through everything that was supposed to feed it, and ask of each one: *are
you actually alive?* The sub waited for a partner that had been dead since
the first instruction.

**Two: a wrong clock is a wrong answer, even if everything else is right.**
The CDC at 60 Hz instead of 75 Hz does not crash. It does not report an
error. It just never finishes, because the sectors arrive at the wrong rate
and the state machine never reaches the state it expects. Timing bugs do
not fault. They wait. And they look exactly like the deadlock you spent a
week chasing.

The Sega CD boots today. Sonic CD plays. The dual-68K fits in 724 KB of RAM
because we XIP the code and recruit every bank. And every time I look at the
sub-CPU's `$6132` spin in an old trace, I remember that the main CPU was
dead ten milliseconds before the sub noticed.
