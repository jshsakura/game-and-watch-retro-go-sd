# gba_m4a — run GBA games' sound mixer natively, and prove it changed nothing

A third of a GBA game's CPU time is not the game. It is the **music**.

Nearly every commercial GBA cart links Nintendo's M4A ("Sappy") sound library, and
M4A mixes its PCM channels **in software, on the guest CPU** — an ARM routine
(`SoundMainRAM`) that the library copies into IWRAM at boot and runs once per
channel per frame. Measured on the real core, with a real ROM:

| | share of ALL guest instructions |
|---|---|
| Final Fantasy Tactics Advance, in-game | **37.1%** |
| Final Fantasy Tactics Advance, menu | 33.7% |
| Pokémon Emerald | 27.3% |

The share barely moves between a busy scene and an idle one, and that is the
whole point: **the mixer's cost is a per-frame constant.** The music mixes the
same amount of audio whatever is on screen. It is not a spike you can skip past —
it is a floor you are standing on.

On a PC that floor is free. On a Game & Watch (Cortex-M7, ~70 host instructions to
interpret one guest instruction) it is the single most expensive thing the
emulator does, and it is why FFTA runs at 0.93×: with **every pixel of rendering
switched off** the interpreter alone still needs 18.02 ms against a 16.74 ms frame.

This package runs that block natively instead of interpreting it, and removes
**29.3%** of the guest instructions the interpreter has to execute — without
changing a single thing the game can observe.

## It is the idle-loop skip's sibling

`idlefind` finds the loop where a game does **nothing**, and skips it. This finds
the loop where a game does **the same thing every frame**, and runs it directly.
Same shape: recognise a known block by its bytes, and execute its *meaning*
instead of its instructions.

The difference is what happens to the clock. The idle skip **deletes** guest
cycles on purpose. This one **charges every one of them** — the block costs the
guest exactly what it always cost, so the game's timeline does not move. Only the
host's work disappears.

## Why no emulator already does this

mGBA and NanoBoyAdvance **both** hook M4A. Neither of them speeds it up.

Both read the channel state and mix natively for **audio quality** — floats, cubic
resampling, reverb, higher sample rates — and then let the guest run its own mixer
anyway (`emulator.cpp`: `if (cpu.state.r15 == hle_audio_hook) { ...SoundMainRAM... }
cpu.Run();`). On a 3 GHz x86, 37% of the guest's instructions cost nothing worth
saving, so nobody saved them.

We are the ones who are poor. So we took their **hook** and left their mixer.

## What is here

| file | |
|---|---|
| `m4a_hle.c` | the native block: a hand transliteration of `SoundMainRAM`, one C statement per ARM instruction, with the original's labels and control flow. Read it next to the disassembly — the shape *is* the argument. Dependency-free. |
| `m4a_hle.h` | the ABI. No gpSP in it, on purpose: the firmware, the prover and any other front-end link **the same file**. |
| `m4a_gpsp.c` | the gpSP adapter: a bus that sees memory exactly as the interpreter does, the IWRAM scan, and the verify mode. |
| `prove.sh` | the proof. Below. |
| `m4a_sigs.c` | **generated** — the variant table. See "the table" below. |

The gpSP fork carries **one `if`** in `cpu.cc` and a per-frame callback in
`main.c`, both behind `GBA_M4A_HLE`.

## The proof

```
./prove.sh <rom.gba> [frames]
```

Two questions, two proofs, and a deliberate failure for each — because a test that
has never failed proves nothing.

**1. Arithmetic.** Every hooked block is run **both ways** from the same state over
the same memory, and every register, every flag, every guest cycle and every byte
either of them wrote is compared. Not sampled — every block the game ran.

```
M4A VERIFY: 5031 blocks identical in registers, flags and memory;
            5031 of them also exact on guest cycles; 0 declined
```

**2. Behaviour.** The whole game is run twice, hook off and hook on, and everything
the guest can see is hashed **every frame**: screen, audio, IWRAM, EWRAM, VRAM,
palette, OAM, I/O, and the cycle counter.

```
identical, all 2000 frames
```

**RED.** Both are re-run with the transliteration deliberately broken (one sample,
one step quieter — nothing crashes, no screenshot changes, nobody would hear it),
and the run **fails** if that is not caught.

### What the proof taught, that reasoning did not

The first version ran the block **atomically**. Its arithmetic was perfect — 11,676
blocks, every register and every cycle exact — and the game still came out
different. The block is ~8,250 cycles long and gpSP runs the CPU in slices of a
scanline or less, so the interpreter does **not** run it in one go: it drops out
eight or ten times on the way through to let `update_gba()` move the video, the
timers and the DMA along. Run it atomically and every one of those hardware events
moves to *after* the music instead of *during* it.

The screen was identical. The audio was identical. **The clock was not** — and a
game that reads a timer would have felt it.

So the block now stops where the interpreter stops — after the same instruction, on
the same cycle — and calls back for more (`m4a_bus::refill`). That is the only
reason the second proof is green.

The bug that made it green was also worth the price of the test: the block's
`push {r4, ip}` had been "optimised" into hand-corrected stack offsets against a
fixed `sp`. It reads the same words and is wrong anyway — because when the block
gives way mid-push, an interrupt taken in that window stacks itself at whatever
`sp` **says**, and the handler wrote straight over the pushed `r4`. Nothing but the
end-to-end hash could have found that.

## The table, and `game-and-what`

The hook needs **no per-game data**. It matches the block's **bytes**: 412 of them,
103 ARM instructions, identical in every game that links that build of M4A. Matching
bytes proves the code we are replacing *is* the code we transliterated. A game code
would be a guess about identity; the bytes **are** identity. An unknown game hooks
nothing, costs one compare, and behaves exactly as before — verified: Pokémon
Emerald (a different, stereo M4A build) interprets **the identical number of guest
instructions** with the hook compiled in.

What *is* worth generating, and what `game-and-what` should own:

- **the variant signatures** — one per M4A build in the wild, discovered by
  sweeping a ROM corpus rather than guessed at. `m4a_sigs.c` is the artifact, in
  exactly the shape `gba_idle_loop.c` already has: generated, copied, never
  hand-edited.
- **the coverage and the receipts** — which carts hook, which variant, how much
  each one saves, and *proof that each was A/B'd bit-exact*. That is a database
  row per game, and it is the same row `idlefind` already writes.

The two tools answer the same question from two ends — *will this cart run on the
real hardware?* — and neither answer is complete without the other. A game with no
idle loop but a hooked mixer is 29% cheaper than the sweep thinks; a game with
both is a game that ships.

## Status

Both variants are proven bit-exact — every block, every register, every guest
cycle, and the whole game frame-by-frame — with the RED for each.

| variant | | guest instructions the interpreter still runs |
|---|---|---|
| `m4a-soundmainram-mono` | FFTA (`AFXJ`) | 76,857 → 55,973 per frame (**−27.2%**) |
| `m4a-soundmainram-stereo` | Pokémon Emerald (`BPEK`) | 42,409 → 27,452 per frame (**−35.3%**) |

The stereo one is the common build (two accumulators, right channel `0x630` past
the left, phase in `r9` instead of `lr`). One path in it is deliberately **not**
transliterated: when a channel's status has bit 4 or 5 set the routine calls a
subroutine that sets a *sample* up — once per note, not once per sample. Those
blocks are declined at the door, before anything is written, and the interpreter
takes them. Emerald: 13 declined out of 8,448.

### The trap that "failed" the stereo variant, and was not the stereo variant

Its end-to-end proof failed at frame 12 — with the screen, the audio, the EWRAM
and the emulated clock all bit-identical, and only IWRAM differing.

**The same binary, run twice, did not agree with itself.** gpSP's RTC takes its
baseline from the *host's wall clock* (`rtc_init_base_time` → `time()`), and
Emerald is an RTC cart: it reads the real time into its own memory during boot. It
was not the mixer. It was Tuesday.

`prove_main.c` freezes `time()`, and `prove.sh` now runs the hook-**off** build
twice and fails if it disagrees with itself, before it compares anything to
anything. A comparator that cannot compare a thing with itself has no business
saying two things differ.

(On the device this is already right: `syscalls.c`'s `_gettimeofday` is backed by
`GW_GetUnixTime()`, the Game & Watch's own RTC, and gpSP reads it once at cart load
and derives the rest from `frame_counter`.)
