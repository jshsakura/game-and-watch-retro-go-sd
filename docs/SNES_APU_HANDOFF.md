# SNES APU — what shipped, what is measured, what is left

Written 2026-08-06, after the release `testbed-full-20260806-2222`.

The July verdict was that the SNES performance axis was exhausted. It was not
quite: the APU had one lever left, it shipped, and the measurements taken on the
way narrow what can still be done. Read this before proposing anything here.

## What shipped

The N-SPC sound driver's main wait — `MOV A,$00FD` / `BEQ -5`, poll timer 0 and
loop while it reads zero — is now charged in one step instead of dispatched. It
is **40% of every SPC opcode** ALTTP and SMW execute over a 700-frame window,
25% over 2500. The read that polls the counter also clears it, so every
iteration before the tick reads zero and clears an already-zero counter: nothing
observable happens in them.

- sm `652d8ed`, branch `perf/spc-idle-skip`; knob `SNES_SPC_IDLE_SKIP` (default 1).
- Gate: state and audio hashes bit-identical, ALTTP and SMW, 700 and 2500 frames.
- Device-shaped: **-3.02% executed instructions/frame (ALTTP), -2.24% (SMW)**,
  STATEHASH/AUDIOHASH unchanged, on `tools/m7_qemu_rig/run_snes_t2.sh`.
- Not yet confirmed on hardware. Expected +1.0-1.4 fps, which is *smaller than
  scene variance* — do not judge it by watching the fps counter. Judge it the way
  the SPC Thumb-2 A/B was judged: profiler build, confirm `cpu_calls/frame` match
  across arms (same scene), then compare bucket absolutes.

## What the measurements closed

- **The Thumb-2 65816 engine handles 255 of 256 opcodes** (only BRK falls back to
  C, plus the decimal-mode ADC/SBC bails). There is no fallback tax to recover;
  "extend engine coverage" is not a lever. Derived from the `DISPATCH_ENTRY`
  macro in `snes_thumb2.S`, not from a guess.
- **The SPC700 opcodes that remain are real work.** After the skip, the hottest
  remaining sites in ALTTP are `$0854-$0862`: `MOV A,!$11ac+Y` / `MOV $00F2,A` /
  `MOV A,!$11b6+Y` / `MOV A,(X)` / `MOV $00F3,A` / `DBNZ Y` — the driver writing
  voice parameters through the DSP address/data ports every tick. Not idle, not
  skippable.
- The residual `$0873/$0876` hits (~10% of remaining opcodes) are the iterations
  the skip deliberately leaves to the interpreter plus the ones cut short by the
  caller's cycle budget. Worth at most a fraction of what was already taken.

## What is open, with its ceiling already measured

**Idle DSP voices.** Zelda runs 2.13 of 8 voices active; 61-66% of all
`dsp_cycleChannel` calls and 53-56% of all BRR block decodes are for voices that
are released and silent (`gain == 0 && adsrState == 4 && !useNoise`). An
ablation that returns early for exactly those voices measures
**-2.74% of executed instructions/frame** on the rig — the same order as the
lever that just shipped.

That ablation is **not correct** and its number is a ceiling, not a result:
STATEHASH stays identical but AUDIOHASH changes, because it drops two observable
things — the pitch counter, and BRR decode's `ENDx` bit, `decodeOffset` and
`previousFlags`. An exact version has to keep those and skip only the 16-sample
filter math, which is unobservable while the voice stays silent *if* key-on
resets the filter state (`old`/`older`, `decodeBuffer`) — that is the fact to
establish first, in the `MY_CHANGES` key-on path, before writing any of it.

Reproduce: `DSP_ABLATE_IDLE` / `DSP_ABLATE_ALL` were temporary edits to
`external/sm/src/snes/dsp.c` and were reverted; re-add them the same way (early
return in `dsp_cycleChannel`, early return in `dsp_cycle`) and run
`RIG_EXTRA_DEF=-DDSP_ABLATE_IDLE bash tools/m7_qemu_rig/run_snes_t2.sh <rom> 800`.

## Do not micro-tune our DSP before reading the alternatives

`external/sm/src/snes/{apu,dsp,spc}.c` is the LakeSnes-style reference
implementation: correct, straightforward, and not written for a 300 MHz
microcontroller. The public work on exactly this problem — blargg's `snes_spc`
fast DSP and SPC700 core, carried by Snes9x and by most constrained ports — is
the thing to evaluate against our numbers before hand-optimizing ours further.
Cost the port against the buckets above rather than assuming it wins.

## The two rigs this used

- `tools/snes_harness/run_spc_probe.sh` — per-PC opcode/cycle histogram of the
  SPC700 plus the ARAM bytes at every hot site. This is what found the lever.
- `tools/m7_qemu_rig/run_snes_t2.sh` — the core on QEMU Cortex-M7 with the
  **Thumb-2 65816 and SPC700 the firmware actually links**. `run_snes_hf.sh`
  compiles both as C and prices an opcode wrong; anything whose payoff is
  "opcodes not dispatched" must be measured on the t2 rig.

Both are described in [HARNESSES.md](HARNESSES.md).
