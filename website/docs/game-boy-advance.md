---
id: game-boy-advance
title: Game Boy Advance
---

# Game Boy Advance

GBA emulation uses **gpSP**, running as an interpreter — there is no dynamic recompiler here,
because gpSP's backends are x86/ARM32/ARM64/MIPS and none of them targets the Cortex-M's
Thumb-2. Every GBA instruction is decoded and executed in software, on a microcontroller with
724 KB of RAM for the whole emulator. Pokémon Ruby and Emerald run at full speed; heavier titles
vary (FFTA sits just under — its interpreter work alone exceeds the frame budget).

Put `.gba` files in `/roms/gba/`; saves land in `/saves/gba/`. Both folders are created on first
boot. No BIOS file is needed — a clean-room replacement ships with the core.

## The ROM never enters RAM

A GBA cart is up to 32 MB. There is no RAM to put it in, so it is not put in RAM: the ROM is
cached into the external QSPI flash and the emulator reads it **where it lies**, memory-mapped. A
16 MB Pokémon ROM costs zero bytes of the RAM budget. The first launch of a game spends a while
writing it to flash; every launch after that is a cache hit and starts immediately.

The one exception is the cart's first 32 KB, which gets a RAM shadow — because an RTC cart
*writes* its clock registers back into "ROM" at `0x080000C4`, and flash does not take writes.
Ruby, Sapphire and Emerald all keep time that way.

## 131 measured idle-loop addresses

A GBA game does its work for a frame and then **busy-waits** for the vertical blank — spinning on
a single branch, doing nothing, sometimes for three quarters of the frame. gpSP can throw those
cycles away, but only if it knows *where* that branch is, and it knows only for the carts in a
hand-maintained table.

A cart absent from that table spins through all 280,896 cycles of every frame. That is not "a bit
slower" — it is the difference between doing 75,000 cycles of real work and pretending to do
280,896.

This build ships **131 addresses measured by running the ROMs**: an address is only kept when the
per-frame cycle count demonstrably collapses with it applied. It is applied over gpSP's own table
rather than by patching it, which also corrects the three entries gpSP has *wrong* — FireRed and
LeafGreen point at an address where there is no loop at all.

(Ruby and Sapphire are deliberately absent. They have no busy-wait to skip: they idle through the
BIOS, which gpSP already fast-forwards. 74% of their frame is rest.)

Some games poll VCOUNT in a loop whose *caller* is the real waiter (Super Robot Taisen D,
Nintendo's tennis engine). Skipping such a poll unconditionally can multiply the caller's work
instead of saving it, so those get a **conditional skip** that only fires while the poll would
keep spinning — measured −15.2% and −16.8% guest instructions with the rendered frames 99.8%
identical. Korean fan-translated carts change the region byte in the ROM header (`BPEK`,
`AXVK`, …), which used to make every per-cart table miss — idle skip, 128K save size, RTC;
entries for the common K-region releases are included.

## The music was a third of the game

Almost every commercial GBA cart ships Nintendo's **M4A** (aka MusicPlayer2000) sound driver, and
that driver **software-mixes its PCM channels on the guest CPU** — every frame, scene-independent,
whether anything moves on screen or not. Measured across six engine variants: **27-60% of all
guest instructions are the mixer**, not the game.

This build recognises the mixer by signature and executes a **native transcription** of it instead
of interpreting it instruction-by-instruction: same registers, same memory, same cycle count,
stopping at exactly the instruction the interpreter would have stopped at (proven bit-identical
against 40,000+ interpreter-executed blocks and 13,000+ frames of lockstep). **347 of 633 tested
carts** — every cart that carries the mixer — hook automatically; the rest simply run as before.
Measured effect: Zelda: The Minish Cap −60%, Emerald −35%, FFTA −27% guest instructions. The
transcribed code lives in external flash (XIP) like the sprite renderer, so it costs the RAM pool
nothing.

## Where the emulator lives

gpSP is 853 KB of code and data. The pool is 724 KB. It is split four ways, and each piece goes
where its access pattern says it should:

| where | what |
| --- | --- |
| ITCM (64 KB) | the ARM7 interpreter — the hottest code there is, a dispatch loop that would miss in flash on every opcode |
| RAM pool | the memory bus, the tile renderer, all of the emulator's state |
| AHB SRAM (120 KB) | the framebuffer, the BIOS image, the cheat table, the sound ring — the things read least, on the slower bus |
| external flash | the sprite renderer, the cold code, and every read-only byte the interpreter never touches |

The interpreter is built `-O3`; it runs from ITCM, so the code it grows by costs nothing but ITCM.
The overclock is raised automatically to the top of the launcher's scale while a GBA game runs
(and lowered again on exit) — and if you have chosen a *higher* level yourself, yours stands. See
[Overclock & power](./overclock-and-power.md).

## Honest caveats

- **Speed is the limit, not compatibility.** Ruby and Emerald hold 60; heavier titles drop
  frames, and FFTA sits just under full speed even with the mixer natively executed. The pause
  menu's **Settings** page reports where the frame actually goes (`Emu+ppu`, `= PPU`, `Scale`,
  `LCD wait`) if you want to see it.
- **Audio is verified by ear on six carts** after the mixer work (plus a resample low-pass tied to
  the cart's mixing rate, and a PSG pitch fix — notes used to come out 5 semitones flat at 48 kHz).
  One intermittent tick remains under investigation; the core keeps a small audio diary it flushes
  to `/gba_audio_diag.txt` on exit to pin it down.
- **No L / R buttons on a Mario unit.** The hardware has no shoulder buttons and no physical X/Y
  either. Games that need L/R are not fully playable there yet.
- **No link cable, no rumble.** Neither exists on this hardware, and both are switched off rather
  than emulated.
