---
slug: snes-60fps-two-measurement-lies
title: "SNES: how a core we'd already declared impossible got to 73fps in simulation — twice by finding out the ruler was wrong"
authors: [jshsakura]
tags: [snes, performance, audio]
---

The launcher already plays three Super Nintendo games superbly — *A Link to the Past*, *Super
Mario World*, *Super Metroid* — because each one is a hand-decompiled native C port, tuned to
that one game. What it can't do is play a `.sfc` file you drop on the SD card. This is the log of
trying to build the generic core that could, hitting a wall that said "impossible," and finding
out the wall was drawn with a bent ruler. Twice.

{/* truncate */}

## The first verdict: no

The generic path is the obvious one — [LakeSnes](https://github.com/elzo-d/LakeSnes)'s
65816 interpreter plus its PPU renderer plus its SPC700, running whatever ROM you hand it,
same code path for every game. The question was whether that could ever hit 60fps across the
library, the way the hand-ported cores already do for their one game each.

Three independent measurements on the QEMU M7 rig — the same instruction-counting harness the
GBA and WonderSwan work leans on — agreed: no. Just the *base* cost of the emulator — CPU
interpreter, renderer, sound chip, the event loop that ties them together — blew past the
60fps instruction budget (5.67M instructions per frame at 340MHz) with the screen black and
the sound off. Nothing drawn, nothing played, still too slow. No amount of frameskip or
audio-muting saves you from a floor that high. And there's no 65816 dynarec anywhere, on any
host architecture, so "just JIT it" wasn't a real option either. The conclusion written down at
the time was blunt: **universal 60fps SNES is not achievable.** File it next to "SuperFX is
infeasible on this MCU" and move on.

I moved on. Then I went back, because the arithmetic itched.

## Lie #1: the rig was 25% too pessimistic, and it was checkable

The QEMU rig had been built soft-float. That's normally invisible — soft-float and hard-float
compute the same answer, just at different speeds — except the interpreter's event loop does
a floating-point multiply-add on *every single opcode* (accumulating APU-catchup cycles), and
a software double-precision FMA on a hot per-opcode path is not a rounding error, it's tens to
low hundreds of extra ARM instructions, times every opcode, times every frame.

The number that gave it away: base cost divided by opcodes-per-frame came out to roughly 519
ARM instructions per 65816 opcode. A plain interpreter dispatch should cost on the order of
100–150. Three to five times too many, on a fixed per-opcode overhead, is exactly the signature
of a software float call hiding in the loop.

The real device runs hard-float (`fpv5-d16`). So I rebuilt the rig hard-float and reran the
exact same save state — literally the same `STATEHASH`, checked before touching anything, so
the *only* variable was the float ABI, not game state, not RNG, not frame timing. Same state,
different arithmetic:

| | soft-float (what I'd trusted) | hard-float (what the device actually pays) |
|---|---:|---:|
| *A Link to the Past*, stock | ~34.2 fps | **44.8 fps** |
| base cost vs. 60fps budget | "37% over budget" | **9.5% over budget** |

A flat ~25% had been inflating every number in this initiative, for weeks, and the "base cost
alone kills it" verdict — the load-bearing conclusion the whole "impossible" call rested on —
turned out to be sitting on a 9.5% gap, not a 37% one. Close enough that the rest of this post
exists.

I want to be honest about what this means for everything I'd written down before this point:
the *shape* of earlier conclusions mostly survived (heavier games were still heavier, PPU was
still expensive), but every absolute fps number needed rescaling, and I don't fully trust any
number from before the rebuild that I haven't since re-measured hard-float. Everything below
is hard-float only.

## Lie #2: the idle-skip lever measured 0%, because I asked the wrong question

Getting the ruler right reopened a door, but it didn't open the one I expected. The next
obvious lever — an idle-skip, the same idea that made the GBA sound-driver work tractable —
came back **0% of gameplay opcodes**. SNES games are interrupt-driven; the detector I'd written
only counted CPU spins polling the APU communication port, and these games mostly don't do
that during gameplay. The GBA titles polled their sound hardware; SNES titles wait for NMI.
By that narrow definition, the lever looked genuinely dead, not just small.

It wasn't dead. It was defined too narrowly. I widened the detector to catch *any* provably
idempotent backward branch — same program counter, same complete register file hash on return,
no memory written, no I/O touched — the general shape of "this loop cannot possibly do
anything except wait for an interrupt, because if it could, some byte somewhere would be
different by the second iteration." That detector found that **81.1% of every executed
gameplay opcode in *A Link to the Past*** is exactly one WRAM flag spin, waiting on the game's
own NMI-complete flag. Super Mario World: 75.6%. Not an APU wait at all — a completely
different address, a completely different reason to be stuck, and by miles the single largest
lever this whole investigation found.

Both dead ends taught the same lesson, and I keep relearning it: **the measurement that closes
a door is the one to distrust hardest**, because it's the one nobody goes back to check once
it's given them the answer they expected.

## What the levers actually buy, once they're real

Everything from here is measured hard-float, and every row is gated on a `STATEHASH` — full
machine state plus audio — identical to the un-optimized baseline. If a lever changes what the
game does or sounds like, it doesn't get to count, full stop.

| Config | insn/frame | fps (rig) | What changed |
|---|---:|---:|---|
| stock (hard-float) | 7.59M | 44.8 | corrected baseline |
| **+ PPU optimization** *(shipped)* | 6.91M | 49.2 | sprite line-candidacy cache + paired blit, PPU cost −49% |
| + 65816→C static translator | 5.98M | 56.9 | opcode dispatch bodies recompiled to C at build time, interpreter kept as fallback |
| + WRAM spin-skip | 5.62M | 60.5 | translator + spin-skip together, 68.4% of opcodes skipped by cycle-accurate replay |
| **+ native N-SPC audio HLE (everything)** | **4.64M** | **73.3** | translator + spin-skip + audio HLE, zero interpreter fallbacks |

The PPU pass is the only row of that table actually shipping right now — sprite evaluation
gets a per-frame line-candidacy cache instead of rescanning all 128 OAM entries per scanline,
and the final blit writes two pixels at a time. It's proven output-identical, not just
"probably fine": five independent bit-exact oracles plus the rig's own state hash, unchanged.
It also lands for free in *Super Metroid*'s hand-decompiled port, since they share the same
`ppu.c` — a good reminder that "generic core" and "existing homebrew ports" aren't as separate
as the org chart suggests.

The translator is the one I find most interesting mechanically: it's not a dynarec, nothing
happens at runtime that wasn't already going to happen. It's a build-time recompiler that
takes the interpreter's own `switch` statement and turns each opcode handler into an inlined C
function, keeping the exact same bus semantics (fetch costs, side effects, everything) — it's
mechanically the same program, just without the dispatch and decode overhead paid fresh on
every single instruction. Four independent gates came back bit-identical, including a build
that forces a 50/50 mix of translated and interpreter-fallback code paths on purpose, just to
prove the seam between them doesn't leak.

The audio HLE deserves its own aside, because I got it wrong once already in this same
investigation. My first pass at measuring it isolated just the SPC700 CPU interpreter — a
fixed ~0.37M instructions per frame, regardless of game, because it's one MHz of one small chip
— and concluded the win was "+3–4%, basically noise." That's true if all you remove is the
CPU. But wiring a native N-SPC player over the interpreter's APU port removes something bigger:
the per-APU-cycle bookkeeping the event loop pays to keep the two processors in lockstep,
about seventeen thousand times a frame. Collapsing that to per-*sample* bookkeeping is where
the real win lives — measured standalone, 49.2 → 57.3fps, and it composes with the
translator+spin-skip stack almost additively rather than fighting them for the same cycles.

## Does any of this hold up across a real library?

One deeply-measured game doesn't tell you whether a lever matters outside a lab. So the survey
and spin tools got pointed at roughly 2,500 ROMs, swept on a remote machine — host emulation and
static analysis, not the device — for boot compatibility, sound-driver identification, and spin
fraction. Full numbers, caveats, and representative titles per tier are in
[`docs/SNES_COMPATIBILITY.md`](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/testbed/docs/SNES_COMPATIBILITY.md);
the headline shape:

- **76.6%** of everything that boots and renders uses some flavor of **N-SPC** — Nintendo's own
  sound engine and the various studios' forks of it — which is the audio-HLE lever's whole
  addressable market. Two of the five known dialects have a validated native player so far
  (`std`, `YI`; that's 49% of N-SPC titles); the rest have recovered ARAM offsets but no proven
  player yet.
- **Median 52.1%** pure-spin across the library, and just over half the titles sit at ≥50% —
  and this genuinely isn't an RPG-specific trick. It shows up in action titles too, wherever a
  game's logic finishes early in the frame and parks waiting for NMI.
- A rough tier split: about 14.5% of the whole library is bootable with *both* levers
  addressable, another 31.5% with one, 26.8% bootable with neither yet, 8.7% needs an
  enhancement chip this core doesn't emulate (SA-1, SuperFX, DSP-1, Cx4, S-DD1), and 18.5% is
  still an open question (didn't render inside the sweep's frame window — that's a "look
  longer," not a verdict).

The spin-skip lever came with its own hard limit, and I want to be specific about it rather than
wave at it: an A/B across that same library — stock versus spin-skip, hashing state and audio —
found **193 ROMs where the skip changes the outcome.** Not slower, *different* — a real
divergence in what the machine does. That's not a rounding error to tune away; running
spin-skip globally on would silently break something on roughly one in ten of the ROMs it
otherwise helps. So it isn't shipping as a global flag. It ships, if it ships, as a per-ROM
whitelist built from exactly this sweep — 1,070 ROMs the sweep can vouch for, the rest excluded
until someone characterizes why they diverge. There's a second data point in the same spirit:
running the (ungated) spin-skip tracker against a game with nothing to skip cost it **27.6fps
versus 38.2fps stock** — the tracker's own bookkeeping overhead, paid for nothing. The lever
that hands Zelda +8fps takes the wrong game backward if you don't ask first whether it applies.

## What's actually in your hands right now

All of the above — translator, spin-skip, audio HLE, the 73.3fps full-stack number — is a
**simulation result**, gated on bit-identical state hashes, not a device measurement. What
shipped in the `testbed-full-20260716-0809` pre-release is deliberately smaller than all of
that: the baseline interpreter plus the PPU optimization, nothing else. That's a ~49fps-class
number in the rig for a Zelda-weight game, and I genuinely don't know what it is on hardware
yet, because I haven't flashed it to a device myself and gotten a number back.

Savestates are wired and I trust them about as much as a host harness can earn: a two-process
cold-resume test — record a save in one process, resume it in a completely fresh one, which is
what a device cold boot actually does and a warm dev loop does not — passes on *A Link to the
Past*, *Donkey Kong Country*, and *Turtles in Time*. Building that harness earned its keep
immediately: it caught a controller shift-register that wasn't part of the saved state (one
title's input desynced exactly one frame after a cold load) and a truncated save file that was
being silently accepted instead of rejected. Both are fixed now, and all three refusal paths
(short file, bad magic, wrong version) are under test.

Cartridges needing an enhancement chip — SA-1, SuperFX, DSP-1, Cx4, S-DD1 — get a rejection
message at load instead of a mid-game hard fault. That's it for what's real today. The
translator needs an XIP delivery story before it can ship at all (the generated code is about
1.33MB, and `RAM_EMU` is 724KB total shared with everything else — it has to execute in place
from external flash, the same pattern *Super Metroid* already uses for its coldest code). The
spin-skip needs its whitelist wired in as a real lookup table. The audio HLE needs its
detect-and-route pipeline built and its LLE fallback proven for the dialects it doesn't cover
yet. None of that is hard in the sense of "unknown," it's hard in the sense of "not done."

## Where this might go, if the device agrees

There's a direction being discussed, not a decision that's been made: the launcher's three
hand-decompiled SNES ports are excellent, but they're also three separate codebases that only
play one game each. If the generic core — fully lettered up, levers and all, and **proven on
real hardware, not in a rig** — can hold a stable 60fps for one of those games, that hand-port
becomes redundant, and the launcher gets simpler for it. *A Link to the Past* is the only
candidate with real headroom in simulation right now (73.3fps against a 60fps bar, an 18%
margin); *Super Mario World*'s generic-core number tops out at 47.0fps today, short of the bar,
because its PPU cost comes from background tile drawing rather than sprites and the sprite-cache
work that helped Zelda didn't touch it; *Super Metroid* is the heaviest of the three and hasn't
even been measured with the levers yet — its existing hand-port only manages 56.2fps on real
hardware, so clearing 60 generically is the hardest version of this problem, and it may simply
stay a hand-port forever. Nothing gets retired on a simulation number. That's the whole point of
this post.

## Honest status

- The pre-release adds a real, flashable SNES tab with the baseline core and working
  savestates — please try it and report an actual fps number, save round-trips, and audio.
  That one number is worth more to this roadmap than anything in the ladder above, because it's
  the conversion factor between "instructions counted in QEMU" and "what the chip actually does"
  — no cache model, no bus-wait model, no DMA contention model went into any number in this
  post.
- The 73.3fps full-stack number, and everything on the way to it, is simulation-proven and
  device-*unverified*. Say it plainly: it is not shipping, and it is not a claim that the
  device does 73fps, or even 60. It's a claim that a particular set of bit-identical
  transformations, measured on an instruction-counting rig, add up to a number comfortably over
  the bar — and that the rig has already been wrong twice in this exact investigation, in both
  directions, until something stricter than "the number looks right" checked it.
- Consolidating the homebrew ports into this core is a direction, gated on the device agreeing,
  game by game. It is not scheduled and nothing is being removed yet.

I keep coming back to the same thing: neither of the two lies in this post was a bug in the
usual sense. Soft-float genuinely computes correct answers, just slowly; the narrow idle-skip
detector genuinely found zero of what it was looking for. Both were *technically correct
measurements of the wrong thing*, and both survived long enough to shape a real conclusion
before something stricter — a bit-identical state hash, a widened detector, eventually a
device — caught them. That's the only process I trust here, and it's the same one I'm handing
off in this post: don't believe the rig, believe the gate, and the device gets the last word.
