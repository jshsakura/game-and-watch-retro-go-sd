---
slug: super-metroid-three-releases-that-couldnt-boot
title: "Three releases of Super Metroid that couldn't boot — and the host harness that swore it was fine"
authors: [jshsakura]
tags: [snes, fault, hardware, savestate]
image: /img/clock-hero.jpg
---

Super Metroid is the port I broke in public the most. Three releases in a row
shipped unable to boot the game, while the host test harness next to them was
reporting **4,000 frames with zero mismatches** against the reference emulator.
This entry is the short version of how a 32-bit microcontroller running the same
code as a 64-bit laptop can be a *different program*, and what it took to notice.

{/* truncate */}

## The host harness was a different program

The first non-booting release is the one that taught me the rule that I have not
broken since: **a host harness that compiles the core's whole source tree is not
testing the firmware — it is testing a different program that looks like the
firmware.**

Two device-only faults hid behind a green host run, and both came from the
firmware's source list being **smaller** than the harness's:

- The firmware excludes `sm_cpu_infra.c`. That file **defines and sets `g_snes`** —
  the pointer the whole SNES register bus goes through. The host harness compiled
  it, so `g_snes` was set, so every register read worked. On the device nothing
  set it. The first read through it was a read of whatever overlay garbage had
  landed at that address.
- The harness built without `-DTARGET_GNW`, so it built itself a **real SPC700**.
  The device has `snes->apu == NULL` — the `spc_player` is the sound chip — and
  the runtime dereferenced it.

A unit test of those functions could not have caught any of it, because the
functions were fine. The wiring was wrong. The host had wired things the device
never wires.

## The fourth release: same program, different CPU

I "fixed" both of the above, shipped a fourth release, and the device booted
straight into a **Hardfault**. Same program, same source list, same defines —
but a 64-bit laptop and a Cortex-M7 are not the same CPU, and the SNES code was
written assuming the laptop.

The fault was in `ClearBackdrop()` in sm's `ppu.c`. It fills a `uint16` buffer
through a `*(uint64*)` cast — eight pixels in one store. On x86 and aarch64 an
unaligned 64-bit store is **legal and fast**. On ARM it is **`STRD`, which
faults unless the address is word-aligned**, and the buffer sat at offset `0x702`
inside `Ppu` — 2 mod 4. The device died on the first rendered line. Four
thousand happy frames on the host had never tripped it.

There is a narrow, evil thing about Cortex-M7 here that hid the bug for even
longer than it should have: **M7 traps *only* 64-bit accesses this way.**
Unaligned halfword and word accesses are legal, and the SNES code does them
constantly — so a host build with `-fsanitize=alignment` *also* passes, because
the sanitizer flags every unaligned access, not just the ones the hardware
traps. The only accesses that matter are the 64-bit ones. Filtering down to
those is what finally made the device's rules enforceable on the host.

And then, in the same week, the same class of bug came at it from the other
side. `spc_player.c` called `ahb_malloc()` with **no prototype in scope**. In C
that means the compiler assumes the function returns `int`. On the 32-bit
device, the truncated pointer is *still the pointer*, so the device ran. On a
64-bit host it is a wild address, and the harness died in `SpcPlayer_Create`
before it reached any emulation at all — so the host caught this one and the
device never saw it. Same lie, opposite direction.

## How the gap is closed now

Three things sit between the firmware and a repeat of this. A new port should
copy all three.

**`tools/sm_harness/device_run.sh`** compiles the core from the Makefile's own
source list — never a copy of it — with the device's defines, and shims the
firmware allocators. It also forces the device's **CPU** rules on the host:
`-fsanitize=alignment` (so the 64-bit violations show up the way M7 sees them)
and `-Werror=implicit-function-declaration` (so a missing prototype is a build
error, not a 64-bit-timebomb). Revert any of the fixes above and it reproduces
the fault on the host.

**A `_Static_assert` next to any type-punned store**, so an alignment
assumption the code makes is one the compiler has to prove, rather than one the
struct layout grants by luck.

**`scripts/check_core_symbol_aliases.py`** runs on every link. Every emulator
core is an overlay linked at the same RAM address, so if core A references a
global that only core B defines, **the linker binds it, quietly**, to B's
address — which, once A is loaded, holds A's own unrelated data. Super Metroid
drove the SNES bus through Super Mario World's `g_snes` for three releases and
asserted on the first register read. The check confirms by disassembly, so dead
references do not trip it; a real cross-core reference fails the build.

## What I actually learned

The bug is usually in the thing that never got wired, not the thing you are
testing. Three of these failures were a caller that never called: Super Metroid
never called `common_emu_frame_loop()`, so no pacing, no frameskip, no FPS
counter; it never called `odroid_system_emu_init()`, so save and load did
nothing at all. No unit test of those functions could have caught any of it,
because the functions were fine.

If your harness is a different program, it proves nothing. And the same program
on a different CPU is still not the same program.
