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

## The device answered, and it changed what to optimize

`testbed-full-20260807-1406` (SPC idle skip + `SNES_LINE_CACHE=1`) on hardware:

| | fps | CPU busy |
|---|---|---|
| A Link to the Past | **51** (used to dip to 40 in heavy scenes) | 92% |
| Super Mario World | **49.5** | 89% |

CPU busy was **99.5%** in every July profile. It is not any more, and the 8-11%
is not headroom lying around: the only place this loop sleeps is
`common_emu_sound_sync()` (`Core/Src/porting/common.c:542-548`), waiting on the
**audio DMA counter**. One DMA period is 266 samples at 16 kHz = 16.625 ms =
exactly one 60 Hz frame. So the idle is frames that finished their work early and
were held at 60 fps — correct behaviour, not waste.

**Therefore the deficit lives in the heavy frames, not the average one.** Average
frame is ~20.2 ms of wall time of which ~18 ms is busy; the light frames are
already at the cap. Optimising a bucket average is now the wrong target — cut the
peak. That is exactly why the line cache won on the device while the host harness
said it cost 4.6%: it removes render work from the worst frames only, and a host
has no memory stalls for a reused line to save.

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

**Sizing the whole DSP bucket needs instrumentation, not ablation.** The obvious
experiment — make `dsp_cycle` return immediately and read the difference — does
not terminate: the guest's own driver depends on the DSP advancing, so the run
never reaches its frame count and QEMU is killed by the timeout. The July split
(spc700 43.7% / dsp 56.3% of the APU bucket) came from an instrumented build,
and `RIG_COST_PROF` on the t2 rig currently reports `spc700=0 dsp=0` because
those tick hooks are not in the tree. Repair those hooks before quoting a DSP
share.

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

## The open question, for whoever takes this next

**What else can be tightened, ranked by what it does to the HEAVY frames?**

The instrument to build first is a frame-time *distribution*, not an average:
`tools/m7_qemu_rig/run_snes_t2.sh` now walks the guest past the title into
gameplay with a scripted pad, so per-frame instruction counts over a real scene
are available — histogram them, find the tail, and attribute the tail to buckets.
A change that halves the p99 frame is worth more than one that shaves 3% off the
mean, and the two are no longer the same trade.

Constraints that are already settled, so nobody spends a day rediscovering them:
the Thumb-2 65816 covers 255 of 256 opcodes (no fallback tax); the SPC timer wait
is already skipped and what remains there is the driver writing DSP ports; the
scheduler is evenly spread with a 1-2% single-optimisation ceiling (July,
disassembly-verified); moving objects into ITCM is a measured net loss; N-SPC HLE
is sealed at 38% breakage; overclock is forbidden by the SD hardware. The one
open lever with a measured ceiling is idle DSP voices, above.

---

# 0808-0809: the device answered, and SMW reached 60

A debug probe (ST-Link on a Tailscale-reachable Raspberry Pi 5) turned every
question in this document from an argument into a measurement. `tools/gnw_probe/`
halts the running console over SWD, reads PC, resumes, and resolves the
addresses through the SNES overlay's own symbols. It also reads `ACTIVE_FILE`
and `g_line_cache_frame`, so a measurement names its own ROM and frame rate.

## What it found in the first screenful

| | Mario Kart | SMW |
|---|---|---|
| `snes_stretch_pull` | **45.9%** | **22.6%** |

More of the machine than the emulator. The hot loop disassembles to `smlalbb`
against `cmp r2,#128`: the WSOLA insertion's autocorrelation, 129 lags of
64-sample MACs, **running inside the SAI interrupt** every time the stream
needed a segment. A melody's period does not change between one frame and the
next eight; `push()` measures it in main-loop context now and the interrupt only
reads it.

**No rig could have found this.** The QEMU rig compiles the core, not the
firmware — the stretcher, `present_frame` and the ISRs are absent from it. That
is precisely why it kept reporting SMW at p50 62.9 fps while the device sat at
59, and why "optimise the peak, not the average" was the wrong conclusion drawn
from the right data.

## Measured on hardware, same save, same place

| | morning | now |
|---|---|---|
| Super Mario World | 49.5 | **60.20** (the audio-DMA pacing cap is 60.15) |
| Zelda 3 | 51 (dips to 40) | **59.40** |
| Mario Kart | 28.3 | **46.38** |

## The knob that was a lie

`SNES_SPIN_SKIP=0` guarded only `external/sm/src/snes/cpu.c`. The device does not
run that interpreter — the Thumb-2 engine's wrapper is `run_one_opcode()` in
`main_snes.c`, and that call site had no guard, so every opcode paid
`spin_engaged()` and `spin_note_real()` regardless. With the flag already at 0,
`spin_note` measured **12.7% of Mario Kart and 8.7% of Zelda**. It is guarded
now. Note what this means for the record: the July "spin tax removed = +3.5 fps"
work could not have touched this call site.

## Open, with numbers

Zelda 3 profile with the learner's C-side hooks off:

```
21.0%  snes_thumb2_step   (the 65816 engine, ITCM)
16.1%  app_main_snes      (inlined scheduler + dispatch loop)
11.0%  cpu_runOpcode
 8.6%  dsp_cycle
 7.3%  run_one_opcode
```

`app_main_snes` splits into two hot spots: `vcvt.f64.s32`/`vmul.f64` — the APU
catch-up accumulator, which is the "scheduler" bucket of July, and Q24 was
already measured a net loss on hard float — and the dispatch loop right after
`bl run_one_opcode`.

**Shipping the spin fix needs its runtime form.** SMW skips 53% of gameplay
opcodes with the learner and wants it; Zelda 3 is off in the per-ROM table and
only pays. Per-game, at runtime, measured on the device.

---

# 0809 afternoon: 51.6 -> 57.3 fps, and none of it was arithmetic

Every gain came from **where code sits**. ITCM is at `0x00000000`, the SNES
overlay at `0x24000000`, and BL reaches +-16 MB — so every call across that line
takes a linker veneer, once per opcode on the hot path. The device profile named
it: `__gsnes__snes_cpuRead_veneer`, 2.7% of the frame.

| | fps |
|---|---|
| morning (gameplay) | 51.6 |
| inline ROM page cache OFF | 54.5 |
| `snes_cpuRead`/`Write` -> ITCM | 55.7 |
| `cpu_runOpcode` -> ITCM | 56.1 |
| `app_main_snes` -> ITCM | 57.1 |
| `cpu_thumb2_read`/`write` -> ITCM | **57.3** |

Deterministic 1800-frame window from reset, three runs each, ±0.05 repeatability.

## The rule, and its counter-example

`apu_run`, `apu_cycle` and `dsp_cycle` went into ITCM too and the console got
**slower** (57.1 -> 56.5): their own callees (`dsp_cycleChannel`,
`spc_runOpcode` -> `spc_thumb2_step`) stay in the overlay, so the move created
more crossings than it removed. **A chain pays only when all of it fits on one
side.** July's "ITCM axis is dead" verdict moved whole *objects*, which cuts a
call graph in half; this moves callees to join their callers.

That also closes the axis: the APU chain ends in `spc_thumb2_step`, 14 KB,
against 4.6 KB of ITCM left.

## Two traps worth the scar tissue

- **`__attribute__((section))` must be on the declaration too.** On the
  definition alone gcc ignores it silently and the function stays where
  `-ffunction-sections` put it. It happened twice before anyone noticed.
- **The inline ROM page cache was a net loss, and repairing it proved it.** Its
  offset constants had gone stale — a `double` added to `Snes` moved
  `romPageBase` from 108 to 112 — so it missed every time. Fixing that cut
  `snes_cpuRead` calls by 73% and the console got *slower* (53.80 vs 54.52).
  Fourteen instructions in the hottest ITCM path cost more than the calls they
  save. The QEMU rig says the opposite (4.01M vs 4.09M instructions/frame)
  because it counts instructions where the device spends cycles. The constants
  are now correct *and* `_Static_assert`ed, and the path they feed is off.

## The tooling, which is half the result

- `GNW_AUTOBOOT=<n>` boots straight into the Nth SNES ROM — by index, because
  the launcher's paths come from FatFs and a literal did not match them (a
  debugger-driven `emulator_get_file()` returned NULL for exactly that reason).
  No one has to be at the console.
- `tools/gnw_probe/bench.sh` times a **fixed frame count from reset**. With no
  input the emulation is deterministic, so frame 1800 is the same frame in every
  build — the only way two arms look at the same thing. Wall-clock readings of
  the same game ranged 51.4 to 60.1 today purely by where the console happened
  to be, which is how "60 fps achieved" got claimed and then withdrawn.

## Open

`dsp_cycle` 9.4%, PPU 9.8%, the APU chain 11% — all in the overlay, none of them
movable within 4.6 KB. 2.9 fps short of the 60.15 pacing cap. The next lever is
not placement.
