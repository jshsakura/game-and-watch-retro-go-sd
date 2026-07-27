# Sega 32X performance histogram guide

> ## ⛔ 이 문서의 도구는 더 이상 트리에 없습니다 (0727)
>
> 32X 코어와 그 리그(`tools/m7_qemu_rig/rig_32x.c`, `run_32x.sh`, 링커 스크립트)는
> **삭제됐습니다** — 판정과 이유는 `docs/32X_CLOSED.md`, 실험 이력 전부는
> `docs/32X_DEVICE_MEASUREMENT_LOG.md`에 있습니다.
>
> 아래의 명령줄은 **그대로는 실행되지 않습니다.** 그럼에도 남기는 이유는 여기 적힌 것이
> 32X 전용 지식이 아니라 **측정 방법론**이기 때문입니다 — 히스토그램으로 핫 루프를 좁히고,
> 게스트 PC에 wall을 귀속시키고, 기기 DWT로 QEMU 명령 수를 반증하는 절차는 다음 코어에서
> 그대로 재사용됩니다. 되살리려면 `git log -- tools/m7_qemu_rig/rig_32x.c`에서 꺼내십시오.

This is the execution guide for the next GLM/Codex session. The goal is not to
guess at optimizations. The goal is to measure a representative 32X workload,
identify the remaining hot paths, prove an optimization is safe, and then
confirm its value on the real device.

## 0. Give this prompt to GLM

Run GLM from this repository root:

```text
/home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd
```

Paste this prompt:

```text
Work from the local testbed branch in
/home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd.

Read docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md completely and follow it in order.
The local testbed history containing commit 93fb0a70 is the integrated 32X
baseline; do not reset, pull over, or replace it with origin/testbed. Create
perf/32x-histogram from the current local testbed HEAD. Do not touch, delete,
stage, or commit the unrelated SegaCD untracked files already in the worktree.
Never add ROMs or profiling logs to git; keep them under /tmp/32x-prof.

First produce the QEMU baseline and frame-cost/SH-2 PC histograms. Do not write
an optimization until the measurements identify a concrete hot loop or phase.
For every code change, run the guide's identical-checksum and multi-ROM gates.
Then add the compile-time-disabled on-device DWT ledger and report QEMU and
device results separately. Keep a running report in
docs/32X_PERFORMANCE_RESULTS.md using the template in the guide. Stop and report
evidence if legal local ROM paths or physical-device access are unavailable;
do not invent measurements.
```

## 1. Ground rules and current baseline

The integrated baseline is the local `testbed` branch at `93fb0a70`. The
Picodrive submodule must be `4bdb3d7d`. The local branch may be ahead of
`origin/testbed`; the local history is intentional.

Start with:

```sh
cd /home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd
git switch testbed
git merge-base --is-ancestor 93fb0a70 HEAD
git submodule update --init external/picodrive
test "$(git -C external/picodrive rev-parse HEAD)" = \
  4bdb3d7d0737e41c7f52990653693a87d579dd68
git switch -c perf/32x-histogram
mkdir -p /tmp/32x-prof
```

Do not run `git clean`, `git reset --hard`, or stage the existing SegaCD
untracked files. Do not use generated `tools/m7_qemu_rig/build/*/rom.32x` as an
input ROM: `run_32x.sh` already byteswapped those files, so feeding one back to
the script byteswaps it a second time.

The 32X wiring is already complete: ROM zero-copy/XIP, SH-2 interpreter,
frontend callbacks, audio/video, save states, and the QEMU state round trip are
present. This task is profiling and optimization, not another wiring pass.

The existing SH-2 fast-loop optimization already handles these two exact
shapes in `external/picodrive/cpu/sh2/mame/sh2pico.c`:

1. `BRA $; NOP` idle spins.
2. One-to-four instruction `NOP`/single-`DT Rn` bodies ending in a backward
   `BF` countdown loop.

Do not implement those again. They are cycle-exact, timeslice-bounded, IRQ
aware, and protected by a negative cache. The negative cache matters: probing
nonmatching hot loops previously cost roughly 4-5% in Kolibri.

Known QEMU improvements from that optimization are approximately 28-44% for
Chaotix, Zaxxon, Virtua Racing, Doom, Virtua Fighter, Tempo, and Metal Head.
Kolibri, Stellar Assault, Star Wars Arcade, NBA Jam, WWF Raw, Primal Rage,
Pitfall, and After Burner had no matching loop and approximately zero gain.
Those no-match games are especially useful for finding the next bottleneck.

## 2. What each profiler can and cannot prove

Use the tools in this order:

| Tool | What it proves | What it does not prove |
|---|---|---|
| QEMU phase profile | ARM Thumb instructions by disjoint PicoFrame phase | Device cache, flash/PSRAM wait states, or real fps |
| QEMU frame histogram | Typical and tail instruction cost per frame | Cortex-M7 cycle cost |
| SH-2 guest-PC histogram | Which guest PCs/opcodes/loop edges execute most | That skipping a loop is timing-safe |
| Device DWT ledger | Actual Cortex-M7 cycles including memory effects | Guest-PC attribution unless a compact PC profiler is also enabled |

QEMU runs with `-icount shift=0`; the rig calibrates the CMSDK timer and
normally reports 40 executed Thumb instructions per timer tick. Treat its
numbers as deterministic A/B instruction counts. Never convert them directly
to fps or claim they equal Cortex-M7 cycles.

At 340 MHz, a nominal budget is about 5,666,667 cycles for 60 Hz and 6,800,000
cycles for 50 Hz. On device, calculate the budget from the actual
`SystemCoreClock / Pico.m.pal`-appropriate frame rate rather than hard-coding
340 MHz.

## 3. Choose a representative ROM corpus

Use legally obtained local ROM paths. Do not commit ROMs, hashes that reveal
private paths, generated ELF files, or raw logs.

Minimum corpus:

- One known fast-loop winner: Doom, Virtua Fighter, Virtua Racing, or Chaotix.
- Two no-match controls: for example Kolibri plus NBA Jam, Star Wars, or
  Pitfall.
- One PAL title or PAL image when available.
- Any game reported to be visibly slow or audio-starved on the device.

Record a SHA-256 fingerprint without copying the ROM into the repository:

```sh
sha256sum /absolute/path/to/game.32x | tee /tmp/32x-prof/rom-hashes.txt
```

Measure gameplay, not only a logo or title loop. The built-in
`RIG_PAD_SCRIPT` presses START around frames 118 and 200, then applies movement
and buttons after frame 260. It may not navigate every title. If necessary,
add a ROM-specific deterministic input script behind a new compile-time flag
and document it.

Use at least 1,200 frames for a stable run. Keep the first 20 frames as warmup,
as the rig already does. Longer runs are encouraged when scene cost drifts.

## 4. Establish the QEMU baseline

For every ROM, use a unique `RIG_OUT`; parallel lanes otherwise overwrite each
other's objects. Save logs outside the repository.

Baseline with normal drawing:

```sh
ROM=/absolute/path/to/game.32x
NAME=game
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT' \
RIG_OUT="/tmp/32x-prof/${NAME}-base" \
RIG_TIMEOUT=1800 \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-base.log"
```

Device-shaped draw-one/skip-two run:

```sh
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT -DRIG_SKIP3' \
RIG_OUT="/tmp/32x-prof/${NAME}-skip3" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-skip3.log"
```

Fast-loop OFF control:

```sh
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT -DGNW_SH2_FASTLOOPS_DEFAULT=0' \
RIG_OUT="/tmp/32x-prof/${NAME}-fastoff" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-fastoff.log"
```

Every accepted run must end in `GATE3 PASS`, have nonzero SH-2 execution, and
show a changing/nonblank framebuffer. Reject data with a pprof refcount-leak
warning. Record the following separately for normal and `RIG_SKIP3` runs:

- Total host instructions/frame, min, max, and first/last 500-frame drift.
- Master SH-2, slave SH-2, 68K, Z80, FM, PWM, sound, MD draw, 32X compositor,
  and `other` phase shares.
- SH-2 host/guest instruction ratio.
- Drawn versus skipped frame averages.
- Framebuffer checksums at the reported checkpoints.

Large `other` means scheduler/event/memory glue needs more attribution. Large
SH-2 host/guest means interpreter dispatch or memory handling is expensive.
Large draw/compositor means guest-loop work is not the first target.

## 5. Add a frame-cost histogram to the rig

The current rig reports average/min/max but not percentiles. Add this only
behind `RIG_FRAME_HIST`, leaving the default build unchanged.

Implementation requirements in `tools/m7_qemu_rig/rig_32x.c`:

1. Store each post-warmup frame's `insn` count in a static `uint32_t` array of
   `RIG_FRAMES` entries. At 1,200 frames this is only 4.8 KiB.
2. Keep drawn and skipped samples in separate arrays or separate views. Mixing
   them produces a misleading bimodal distribution.
3. Sort a copy and print `p50`, `p90`, `p95`, and `p99`, plus min/max.
4. Print 20 fixed bins. Derive bin index with 64-bit arithmetic to avoid
   overflow: conceptually `(value - min) * 20 / (max - min + 1)`.
5. Print sample count and bin percentage. Do not print one line per frame by
   default.
6. Exclude the existing 20 warmup frames exactly as average calculation does.
7. Compile and run both with and without `RIG_FRAME_HIST`; the framebuffer
   checkpoints and `GATE3` result must match.

Invoke it with:

```sh
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT -DRIG_FRAME_HIST' \
RIG_OUT="/tmp/32x-prof/${NAME}-framehist" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-framehist.log"
```

Optimize for p95/p99 budget misses, not just the average. A lower average with
a worse p99 can still produce audio underruns and visible frame pacing issues.

## 6. Add the SH-2 guest-PC histogram

Add a separate `RIG_SH2_PC_HIST` compile-time mode in
`external/picodrive/cpu/sh2/mame/sh2pico.c`. It is QEMU-only instrumentation;
do not enlarge the release firmware's BSS.

### Hook location

Count after `sh2->ppc` and `opcode` are established and before
`gnw_sh2_fastloop()` and the opcode switch. Count master and slave separately
using `sh2->is_slave`. Also record whether the instruction came from direct
fetch or a branch delay slot; delay-slot PCs must not be classified as normal
loop heads.

Run the first histogram with fast loops disabled. Otherwise fast-forwarded
iterations intentionally disappear from the histogram and hide the loops they
eliminate. Run it again with fast loops enabled to expose the residual hot set.

### Storage

For QEMU, use an exact sparse open-addressed table per SH-2:

- 8,192 slots per core.
- Key: 32-bit guest PC plus an occupied marker.
- Value: 64-bit execution count; optionally direct/delay counts.
- Linear or bounded probing with a collision/overflow counter.
- Approximately 192 KiB total is acceptable in the rig's 4 MiB RAM but must
  remain excluded from the device build.

Do not use a direct 24/32-bit address array. Report table occupancy,
collisions, and dropped samples; a histogram with dropped hot samples is not
evidence.

At the end of the run, print the top 50 entries for each core:

```text
core  pc        opcode  direct      delay       total       percent  mnemonic
M     06001234  affe    12345678    0           12345678    22.4%    bra ...
```

`external/picodrive/cpu/sh2/mame/sh2dasm.c` provides `DasmSH2()` if a mnemonic
is useful. If adding that source complicates the trimmed rig, print opcode and
the eight words before/after the PC, then disassemble offline. Do not change
the production source list only for pretty profiler output.

Also record one of these loop-oriented views, because a hot leaf PC alone does
not define a safe optimization:

- Top `(previous_pc, current_pc)` transitions, or
- Counts for taken backward branch PCs and their targets.

Use bounded storage and report overflow for this view as well.

Run commands:

```sh
# Full, unskipped guest execution: reveals loops removed by the existing lever.
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT -DRIG_SH2_PC_HIST -DGNW_SH2_FASTLOOPS_DEFAULT=0' \
RIG_OUT="/tmp/32x-prof/${NAME}-pchist-off" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-pchist-off.log"

# Residual hot set after the existing lever.
PHASE_PROF=1 \
EXTRA_DEF='-DRIG_PAD_SCRIPT -DRIG_SH2_PC_HIST' \
RIG_OUT="/tmp/32x-prof/${NAME}-pchist-on" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 1200 \
  | tee "/tmp/32x-prof/${NAME}-pchist-on.log"
```

### Candidate acceptance rules

A candidate should meet all of these before implementation:

- It is at least roughly 3-5% of guest instructions in a gameplay window, or
  has comparably strong host-phase evidence.
- It appears in more than one window; ideally in more than one ROM.
- The exact instruction sequence and control-flow edge are known.
- Memory/IO behavior, IRQ visibility, cycle accounting, delay-slot behavior,
  and timeslice boundaries are understood.
- There is a conservative estimate of removable host instructions/frame.

Reject fast-forwarding for loops that read or write memory-mapped IO, poll
communication/timer/VDP/PWM/FIFO state, can observe an interrupt mid-loop, or
contain side effects that are merely inconvenient to model. Reject title-only
hotspots and changes whose gain is within about 1% QEMU noise/overhead.

Rank opportunities with a conservative upper bound:

```text
possible saving ~= total phase share * hot-path share * removable host cost
```

This is prioritization only, not a performance claim.

## 7. Correctness gate for any interpreter optimization

For A/B, hold constant ROM fingerprint, frame count, input script, compiler
flags, and warmup. Use different `RIG_OUT` directories.

Required per-ROM checks:

1. Baseline and candidate both end in `GATE3 PASS`.
2. Reported framebuffer checksums match at all checkpoints.
3. Average, percentile, phase, and SH-2 host/guest changes are reported.
4. Repeat with `RIG_SKIP3`.
5. Run the save/load round trip after interpreter or timing changes:

```sh
EXTRA_DEF='-DRIG_PAD_SCRIPT -DRIG_STATE_TEST' \
RIG_OUT="/tmp/32x-prof/${NAME}-state" \
bash tools/m7_qemu_rig/run_32x.sh "$ROM" 600 \
  | tee "/tmp/32x-prof/${NAME}-state.log"
```

If checkpoints differ, use `-DRIG_TRACE_CKS` only long enough to find the
first divergent frame. Do not accept a visually similar result. After a
single-ROM proof, run the full available 15-ROM corpus or an equivalent broad
set. A new interpreter shortcut must preserve behavior in no-match controls,
not merely accelerate its discovery ROM.

Keep a runtime kill switch and a compile-time default for every speculative
lever. The disabled path should be behaviorally identical and should not tax
ordinary dispatch.

## 8. Confirm on the physical device with DWT

QEMU chooses candidates; device DWT decides whether they matter on STM32H7.
The helpers already exist in `Core/Src/porting/common.c` and
`Core/Inc/porting/common.h`:

```c
common_emu_enable_dwt_cycles();
common_emu_clear_dwt_cycles();
common_emu_get_dwt_cycles();
```

Add instrumentation to `Core/Src/porting/md32x/main_md32x.c` only behind a
disabled-by-default `MD32X_DEVICE_PROFILE` define. Use `uint64_t` accumulators.
Per-frame 32-bit DWT deltas are safe; the raw 32-bit counter wraps in roughly
12.6 seconds at 340 MHz, so do not use one unbroken multi-second delta.

Use disjoint buckets:

- `PicoFrame` total.
- Input/front-end work before `PicoFrame`.
- Overlay plus `lcd_swap` for drawn frames.
- Audio submit/synchronization.
- Wait/pacing after useful work.
- Whole main-loop total.

If deeper PicoDrive phase probes are added, pause the enclosing bucket or
subtract children exactly once. Nested elapsed times must not be summed as if
they were disjoint.

Keep separate histograms for drawn and skipped frames and print p50/p90/p95/p99
cycles, budget cycles, and over-budget count. Report actual `SystemCoreClock`,
PAL/NTSC mode, overclock level, firmware commit, and optimization switch state.

Do not write profiling data to SD during active emulation. The existing
`/32x_diag.txt` path is sealed before the main loop, and reopening/writing
during play risks pacing distortion and card corruption. Accumulate in RAM and
either render a summary on screen or pause emulation/audio and perform one
controlled write on an explicit key chord. Refresh the watchdog around a
controlled dump.

Measure baseline and candidate from the same save-state/gameplay position,
with the same clock, volume, frameskip behavior, and display settings. Run
multiple windows. A useful result lowers actual DWT p95/p99 or over-budget
count; a QEMU-only instruction reduction is insufficient.

## 9. Build and repository gates

After changing the Picodrive source list or globals, regenerate overlay symbol
renames and require no unexpected diff:

```sh
tools/gen_md32x_redefines.sh
git diff --check
```

Build with the repository's documented Docker release command:

```sh
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
  DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 \
  ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

If Picodrive changes, commit and push the submodule branch first, then update
and commit the superproject gitlink. Do not point the superproject at an
unpublished submodule commit. Never stage ROMs, `/tmp` logs, generated rig
objects, or unrelated SegaCD files.

## 10. Results file and decision format

Create `docs/32X_PERFORMANCE_RESULTS.md`. For each experiment use:

```markdown
## <candidate or baseline name>

- Firmware commit:
- Picodrive commit:
- ROM SHA-256 / region:
- Frames / warmup / input mode:
- Defines and fast-loop state:
- Device clock and PAL/NTSC, if applicable:

### Correctness

- GATE3:
- Framebuffer checkpoints A/B:
- State round trip:
- Corpus pass count:

### QEMU

| metric | baseline | candidate | delta |
|---|---:|---:|---:|
| host insn/frame | | | |
| p50 | | | |
| p95 | | | |
| p99 | | | |
| SH-2 host/guest | | | |

### Hot paths

| core | PC/opcode | share | loop classification | safety notes |
|---|---|---:|---|---|

### Device DWT

| metric | baseline cycles | candidate cycles | delta |
|---|---:|---:|---:|
| PicoFrame p50 | | | |
| PicoFrame p95 | | | |
| PicoFrame p99 | | | |
| over-budget frames | | | |

### Decision

- Keep/reject:
- Measured benefit:
- Remaining risk:
- Next highest-value candidate:
```

Do not report percentages without absolute values and sample counts. Clearly
label inference versus measurement.

## 11. Completion criteria

The investigation is complete only when it has:

1. Reproducible phase and frame percentile baselines for the selected corpus.
2. Master/slave SH-2 top-PC and loop-edge histograms with overflow accounting.
3. At least one ranked candidate, or evidence that the bottleneck is outside
   SH-2 guest loops.
4. Identical-checksum QEMU A/B results and the state round-trip gate.
5. A broad multi-ROM regression run for accepted interpreter changes.
6. Physical-device DWT results, or an explicit `blocked: no device access`
   statement without fabricated numbers.
7. A concise keep/reject decision in `docs/32X_PERFORMANCE_RESULTS.md`.

If the data says draw, compositor, audio, scheduler, or memory wait is the real
bottleneck, stop searching for another SH-2 loop shortcut and instrument that
phase next.
