# SNES peak-frame analysis — the tail is the only lever

Written 2026-08-07, after the release `testbed-full-20260807-1406`.

The premise changed. `SNES_APU_HANDOFF.md` established that the 8-11% idle is
`common_emu_sound_sync()` parked on the audio DMA counter — one period is
16.625 ms = one 60 Hz frame — so the light frames are already at the cap and the
deficit lives in the heavy ones. This document is the per-frame distribution
that premise asked for, the LINE_CACHE's effect on that distribution, and the
single lever still open.

Read `SNES_APU_HANDOFF.md` first. The closed axes listed there (Thumb-2 65816
255/256 coverage, SPC timer wait skip, scheduler, ITCM object moves, N-SPC HLE,
overclock) are closed here too. Do not re-litigate them.

## Method

All numbers are from `tools/m7_qemu_rig/run_snes_t2.sh` — the rig that compiles
the device's actual Thumb-2 65816 + SPC700 engines and runs QEMU `-icount
shift=0`, so the timer delta **is** the instruction count. `run_snes_hf.sh` is
VOID: it compiled the engines as C and miscounted opcodes.

The rig emitted window averages (200-frame buckets) until this session.
`RIG_FRAME_DIST` (new in `tools/m7_qemu_rig/rig_snes.c`) records per-frame
`(t1-t0)` for every frame and prints min/p1/p5/p10/p25/p50/p75/p90/p95/p99/max
split by phase (0-299 boot/title, 300-599 transition, 600-899 early gameplay,
900+ gameplay). FPS projections are `312,000,000 / insn`.

Boundary (per `docs/SNES_COST_BATCH.md`): QEMU reports executed instructions, not
STM32H7 cache/XIP stalls. The rig is an **optimistic ceiling** — device fps ≤ rig
projection. The device measured 51 fps with LINE_CACHE shipped; the rig projects
p50 = 66.5 fps. That ~25% gap is device overhead (cache misses, bus contention,
blit/DMA/OS), and it is why the rig is a *direction finder*, not a *promise*.

## Baseline: every heavy frame is the same weight

**Test A** — baseline (no LINE_CACHE), static input (Link standing), ALttP, 1200
frames. STATEHASH=`1d0d959d`, AUDIOHASH=`c70e2bdf`.

| EMU 900+ | insn/frame | fps @312M |
|----------|-----------:|----------:|
| min      | 7,598,480  | 41.1      |
| p50      | 7,734,560  | 40.3      |
| p90      | 7,838,600  | 39.8      |
| p95      | 7,862,480  | 39.7      |
| p99      | 7,901,560  | 39.5      |
| max      | 7,905,760  | 39.5      |

**The distribution is flat.** p50 and p99 are within 2% of each other; min and
max within 4%. There is no spike to chase — the average frame *is* the heavy
frame. Every gameplay frame does the same amount of work: full PPU render of a
screen that barely changed, full DSP mix, full 65816 interpretation. This is why
no bucket-average optimisation can move the device number by more than its own
measured percent: there is no tail to lift, only a wall to lower.

## LINE_CACHE: shifts the wall, leaves a tail

`SNES_LINE_CACHE=1` (shipped in `testbed-full-20260807-1406`) skips
`PpuDrawWholeLine` for scanlines whose PPU state + VRAM/CGRAM/OAM dependencies
match the cached copy. The cache key excludes two write-only fields that the
game's VBlank DMA churns every frame but that do not affect rendered pixels:
`oamAdr` (the OAM write pointer — render scans all 128 entries independently of
it) and `m7matrix` (mode 7 affine matrix, guarded so the exclusion only applies
when neither frame was mode 7). Without those exclusions the predictor catches
0% of reusable lines; with them, **96.61%** during static gameplay and
**0 false positives** across the full run (probe in `SNES_LINE_REUSE_PROBE`,
variant `excl_oamadr_cgram_line`). Cooldown was removed (was forcing relearn
every 18 frames, capping hit rate at ~76%).

**Test B** — LINE_CACHE, static input, ALttP, 1200 frames.
STATEHASH=`1d0d959d`, AUDIOHASH=`c70e2bdf` — **identical to Test A**.
Cache hit rate: 96.61% gameplay, 84.00% overall.

| EMU 900+ | insn/frame | fps @312M |
|----------|-----------:|----------:|
| min      | 4,474,120  | 69.7      |
| p10      | 4,509,560  | 69.2      |
| p25      | 4,531,960  | 68.9      |
| p50      | 4,617,760  | 67.6      |
| p75      | 4,714,920  | 66.2      |
| p90      | 5,237,320  | 59.6      |
| p95      | 5,420,680  | 57.6      |
| p99      | 8,017,400  | 38.9      |
| max      | 8,523,680  | 36.6      |

**The distribution is bimodal.** 90% of frames sit at 4.5-5.2M insn (60-68 fps);
10% jump to 5.4-8.5M (37-58 fps). p90 lands exactly at 60 fps. The tail is cache
misses — frames where VRAM/CGRAM writes invalidated enough lines to force full
re-render plus the dependency-check overhead.

The bottom percentiles (min-p10) are slightly *slower* with the cache than
without (4.47M vs ... actually they don't exist in baseline, because baseline
has no light frames). This is irrelevant — those frames are already 69+ fps.
What matters is that **max went up 7.3%** (8.52M vs 7.91M): the worst cache-miss
frame pays the dependency-check cost *on top of* a full PPU render. That is the
price of the cache, and it is worth paying because it is one frame, not the
average.

**Active input** (Test D: D-pad cycling + A button after frame 900) produces a
near-identical cache distribution — p50=66.5, p90=59.8, p99=38.9 — because
active input in this test led Link to a simpler scene (baseline median dropped
from 7.7M static to 5.7M active). The cache hit rate held at 93.80%, only 2.8
points below static. STATEHASH/AUDIOHASH identical between cached and baseline
active runs.

## Mode 7 — bit-identical on device, bypassed (zero overhead)

### Device/host verification (user-run, real Mode 7 content)

Super Mario Kart (HiROM, DSP-1) was run on the user's host build with **live
Mode 7 rendering** (lit=56643 — full race screen, not the stuck title):

| LINE_CACHE | STATEHASH | AUDIOHASH | lit | host ms/frame |
|------------|-----------|-----------|----:|--------------:|
| OFF        | 949681c3e85218d1 | 8444dc283e0c4a0b | 56643 | 0.767 |
| ON         | 949681c3e85218d1 | 8444dc283e0c4a0b | 56643 | 0.909 |

**Bit-identical across 1200 frames of real Mode 7 rendering.** No ghosting, no
corruption, no dropped pixels. The cache is safe on Mode 7 content.

But the cost is **+18.5% host time** (0.767→0.909 ms/frame) for **0% reuse** —
Mode 7's affine transform changes `m7matrix` every line, so every line misses.
The device has no stall reduction to offset this, so SMK would be slower.

### Fix: bypass the cache entirely when Mode 7

The +18.5% overhead is unconditionally pure waste — Mode 7 *always* produces 0%
hit rate. One-line fix at the call site (`ppu.c:961`):

```c
bool cache_eligible = ppu->mode != 7 && PpuLineCacheBeginLine(line - 1);
```

When `mode==7`: `cache_eligible=false` → no VRAM tracking, no capture, no
compare, no commit → straight to `PpuDrawWholeLine`. Zero overhead.

**Rig verification after bypass:**

| ROM | STATEHASH | AUDIOHASH | hit rate (gameplay) | w1200 emu |
|-----|-----------|-----------|--------------------|----------:|
| ALttP (non-M7) cache ON | 1d0d959d | c70e2bdf | 96.61% | 4,741,731 |
| ALttP baseline reference | 1d0d959d | c70e2bdf | — | 4,730,741 |
| SMK (Mode 7) cache ON | 232b7226 | a5820c68 | bypassed (no output) | 4,029,044 |
| SMK baseline reference | 232b7226 | a5820c68 | — | 4,030,000 |

ALttP: cache preserved, +0.23% from the extra condition check (negligible).
SMK: no `[line-cache]` output at all = cache fully bypassed, no overhead.
Both bit-identical to their baselines.

### QEMU rig limitation

SMK does not boot in QEMU (lit=418 constant, SPC700 boot handshake timing).
Git history (`fb3f32d3`, `869b7c9e`, `8f3ff657`, `c2fca298`) proves SMK boots
and races on device with DSP-1 HLE + Mode 7 PPU. The user's host verification
above is the real-surface evidence; the rig can only confirm the bypass
produces no `[line-cache]` activity and bit-identical hashes on the static
title state.

## Cache-machinery overhead — word-wise RegsMatch (-5.2%)

User directive: use the rig to find 5% more workload savings. The cache ships
~499K insn/frame of overhead (10.8% of the median cached frame) — pure
dependency-check cost on top of the 4.14M CPU/DMA/IRQ floor.

**Measurement (m0311).** A/B: LINE_CACHE vs LINE_CACHE+`RIG_FRAMESKIP`
(CPU+APU only, PPU entirely off). FRAMESKIP p50=4,131K, LINE_CACHE
p50=4,630K. Delta = 499K = 10.8% of median frame. Split across 224 lines,
that is ~2,228 insn/line for the cache check.

**Lever A — word-wise `PpuLineCacheRegsMatch` (shipped, -5.2%).** The
original implementation compared the cache state byte-by-byte (120 iterations
× two range checks per byte, for `oamAdr` and `m7matrix` exclusion). Replaced
with a `uint32_t` stride loop: 30 iterations, one range check per word, with
the m7matrix word range and oamAdr byte mask computed once via `offsetof`.
`_Static_assert` guarantees the struct size is word-aligned.

Result (m0324): STATEHASH=`1d0d959d` AUDIOHASH=`c70e2bdf` **identical**, hit
rate 96.61% unchanged, emu w1200 = 4,483,906 (was 4,730,741 = **-5.2%**).

**Lever B — skip `ppu_evaluateSprites` on no-candidate lines (shipped,
negligible).** `ppu_evaluateSprites` runs at line 968 BEFORE the cache check
at 976. On cache-hit lines (96.6%), the `objBuffer` it produces is never
read — `PpuDrawWholeLine` is skipped. The fast path now checks
`objLineCand[y][0..3] == 0` (already maintained per-frame by
`ppu_rebuildSpriteLineCache`) and sets `lineHasSprites=false` directly,
deferring the full sprite eval to cache-miss lines.

Safe because `PpuDrawWholeLine` gates ALL sprite compositing on
`if (ppu->lineHasSprites)` — `objBuffer` is never read when false, and the
dep check guarantees no OAM change on hit lines.

Result (m0330, combined A+B): STATEHASH=`1d0d959d` **identical**. EMU
gameplay p50=4,369K (71.4fps), p90=4,989K (62.5fps). Delta from A alone is
negligible — `objLineCand` is rarely all-zero during ALttP gameplay (Link is
always on screen). The mechanism is correct and bit-identical; its yield is
ROM-dependent.

**Lever C — defer VRAM-tracking memset to miss-only (reverted — broke
bit-identical).** Moving `memset(g_line_cache_cur_vram, 0, ...)` from before
the cache check to inside the `!reused` branch produced
STATEHASH=`2feccd92` (was `1d0d959d`). Root cause: `g_line_cache_cur_vram`
is populated by `PpuLineCacheVram` calls during CPU/DMA VRAM writes between
lines; the timing of the memset relative to the CPU execution phase matters
for dep tracking. Reverted.

| Combined A+B (ALttP 1200f) | insn/frame | fps @312M |
|----------------------------|-----------:|----------:|
| EMU 900+ min               | 4,228,000  | 73.8      |
| EMU 900+ p50               | 4,369,000  | 71.4      |
| EMU 900+ p90               | 4,979,000  | 62.7      |
| EMU 900+ p95               | 5,161,000  | 60.5      |
| EMU 900+ p99               | 7,734,000  | 40.3      |
| EMU 900+ max               | 8,236,000  | 37.9      |

APU constant ~544K insn/frame, not a bottleneck. CPU+DMA+IRQ floor = 3,587K.

## Dispatch-floor profiling — the CPU is the wall

User directive (m0363): find 1-2 more levers in the floor. The floor (CPU + DMA +
IRQ + dispatch) was profiled end-to-end via `RIG_CALL_PROFILE`, which instruments
every dispatch entry point with per-window counters.

### Call profile (ALttP 1200f, gameplay w1200)

| subsystem                 | calls/frame | cost (insn/frame) | notes |
|---------------------------|------------:|------------------:|-------|
| 65816 opcodes             |    13,119   |      ~3,010,000  | 70% of frame. ~231 QEMU insn/opcode. Irreducible. |
| `snes_cpuRead`            |     7,915   |         ~93,000  | 91% WRAM fast path, 7.4% ROM page cache, 1.2% slow path |
| `snes_cpuWrite`           |         —   |          (small) | not printed separately; dwarfs under opcode cost |
| `dma_cycle`               |     1,051   |          ~4,200  | **2 return true per frame**. DMA work is negligible. |
| `dma_doDma` (VBlank bulk) |         2   |          ~4,000  | one per VBlank, drains synchronously |
| `dma_doHdma`              |       225   |         ~18,000  | once per scanline. 225 = ~active scanlines. |
| `apply_irq_match`         |    13,042   |         ~52,000  | 99.6% early-return (12,994 skip), 0.37% match (48). |

DMA is not a meaningful cost — 1,051 calls, only 2 of them do work. IRQ dispatch
is 99.6% early-return and costs ~52K (1.2% of frame). The 3,587K floor is
dominated by the CPU interpreter at ~3,010K (84%).

### DMA-check hoist in `run_dots` (shipped, -1.5%)

`run_dots` (main_snes.c:162-209 / rig_snes.c:226-281) checks
`snes->dma->dmaBusy || snes->dma->hdmaTimer > 0` at the top of every iteration —
once per opcode, ~13K times/frame. The check is 2 DTCM loads + OR + branch = ~4
cycles on M7. DMA is active only during HDMA countdown (hPos 1024→1362), so the
check misses 99.99% of the time.

Hoisted `dma_active` into a local register, refreshed only after opcodes that
*started* a DMA (writes to DMAEN at $420b, which drain synchronously inside
`snes_writeReg`). In the common no-DMA case the hot loop now does a single
predicted-not-taken branch on a register.

Safety proof: `dma_active` can go true only via (1) an opcode writing DMAEN
$420b, which `snes_writeReg` drains synchronously so the flag is observable
immediately after; (2) `hdmaTimer` increase, which happens only in
`dma_doHdma`/`dma_initHdma` called from `snes_handle_pos_stuff` — *outside*
`run_dots`.

| ALttP 1200f, RIG_FRAME_DIST | before | after | delta |
|------------------------------|-------:|------:|------:|
| EMU 900+ p50                 | 4,369K | 4,304K | -65K (-1.5%) |
| EMU 900+ p90                 | 4,979K | 4,924K | -55K (-1.1%) |
| EMU 900+ p99                 | 7,734K | 7,705K | -29K (-0.4%) |

STATEHASH=`1d0d959d` AUDIOHASH=`c70e2bdf` **identical**. The device saving is
likely *larger* than 1.5% — QEMU does not model the two eliminated DTCM loads,
which on M7 cost 2-6 cycles each.

Applied to both `rig_snes.c` (for measurement) and `main_snes.c` (device
`run_dots` — this is the emulation engine, *not* the user's pacing block at
lines 1092-1193).

### IRQ-enabled hoist — neutral, reverted

Same pattern attempted on `apply_irq_match`: hoist
`bool irq_armed = hIrqEnabled || vIrqEnabled` out of the per-opcode check and
refresh it after each opcode. The check is the same shape (2 struct loads + OR
+ branch), so the refresh costs the same as the early-return it removes.

Result: STATEHASH identical but p50 = 4,387K (+1.9% WORSE). The extra local
variable bookkeeping and the refresh outweigh the saved early-return on QEMU.
**Reverted** on both rig and device. `apply_irq_match` is already optimal — its
99.6% early-return is two loads and a branch, which is the cheapest possible
gate.

### Floor decomposition after all work

The 4,304K median cached frame is now:

| component                            | insn/frame | share |
|--------------------------------------|-----------:|------:|
| 65816 interpreter (`cpu_runOpcode`)  | 3,010,000  | 69.9% |
| cache machinery (check + capture)    |   434,000  | 10.1% |
| APU (SPC700 + DSP, end of frame)     |   544,000  | 12.6% |
| dispatch (DMA hoist, IRQ, etc.)      |   316,000  |  7.4% |

Of these, only the cache machinery is reducible (already at word-wise compare).
CPU, APU, and dispatch are at their floors. **Levers exhausted.**

## What the cache does NOT do

- **HDMA-heavy games.** Per-line state capture handles HDMA correctly — it just
  lowers the hit rate. Not a correctness risk.
- **The CPU.** The cache only touches PPU compositing. The 4.14M insn/frame of
  65816 + DMA + IRQ is untouched. That is now the floor: even a zero-cost PPU
  would still only reach 312M/4.14M = 75 fps.

## The DSP idle-voice lever — measured, negligible

Zelda uses 8 DSP voices but only ~2.13 are active on average; 61-66% of
`dsp_cycleChannel` calls and 53-56% of BRR decode time are spent on voices in
`gain==0 && adsrState==4` (released/silent). The existing optimisation (dsp.c
line 296-301) skips `dsp_getSample` for those voices and is bit-identical and
shipped.

What was **not** shipped: also skipping `dsp_decodeBrr` for idle voices. The
`SNES_APU_HANDOFF.md` ablation measured **-2.74% insn/frame** but AUDIOHASH
changed — the drop was pitch counter, ENDx bit, decodeOffset, previousFlags,
and `old`/`older` filter samples, all of which `dsp_decodeBrr` advances even
on a silent voice.

**This session implemented and measured the safe version.**
`SNES_DSP_BRR_IDLE_SKIP` (dsp.c) adds `dsp_decodeBrrIdle`, which advances BRR
state (`previousFlags`, `decodeOffset`, loop/end handling) but skips the
16-sample decode loop and freezes `old`/`older`. The freeze is harmless iff
the first BRR block after every key-on has `filter == 0` (because filter=0
never reads `old`/`older`). A probe (`RIG_DSP_KEYON_PROBE`) on ALttP logged
341 key-on events — **all 341 (100%) first BRR blocks use filter=0**. The
freeze is safe for ALttP.

| ALttP 1200f, LINE_CACHE + … | w1200 emu | AUDIOHASH |
|-----------------------------|----------:|----------:|
| nothing (LINE_CACHE only)   | 4,730,741 | c70e2bdf  |
| DSP_BRR_IDLE_SKIP           | 4,720,121 | c70e2bdf  |
| **delta**                   | **-10,620 (-0.22%)** | **identical** |

The -2.74% ablation ceiling assumed the *entire* idle-voice path could be
eliminated. The safe version still reads the BRR header, advances
`decodeOffset` past the 9-byte block, checks loop/end flags, and copies the
interpolation tail — only the 16-sample decode + filter is skipped. That loop
is ~100 instructions per voice per BRR block, and with ~5.87 idle voices at
the SMK/ALttP pitch rate, the saving is noise. **This lever is closed.**

## Device vs rig — the 51fps reading

The rig projects p50 = 66.5 fps with the cache; the device reads 51 fps. The
gap is real and it is not the cache's fault. The rig counts instructions and the
device runs them against memory:

- Every VRAM read, CGRAM read, and framebuffer write the cache eliminates is a
  bus transaction the device no longer makes. The cache should do *better* on
  device than on rig, proportionally.
- But the device also pays for blit (DMA2D + non-cacheable framebuffer), audio
  DMA, SD card IRQ latency, and the launcher's per-frame housekeeping. Those are
  constant costs the rig does not model.
- The 51 fps device reading has CPU busy at 92%, so the remaining 8% is
  `common_emu_sound_sync()` waiting on the audio DMA — i.e. those frames are at
  the 60 fps cap and idle. The deficit is concentrated in the other 92%, which
  is exactly where the cache works.

To move the device number past 51 fps, the rig's per-frame distribution says the
work is in two places: (1) the 10% cache-miss tail (p99=7.7M, 40.3 fps projected
— these are the frames that dip below 60 on device), and (2) the untouched 3.59M
insn/frame of CPU/DMA/IRQ, which is the floor no PPU optimisation can reach.

## Closed axes — do not re-open

These were closed by device measurement, not by argument. See
`SNES_APU_HANDOFF.md` and `docs/OPTIMIZATION_LEDGER.md` for the receipts.

- Thumb-2 65816 (255/256 opcodes, BRK + decimal mode fall back to C).
- SPC700 timer wait skip (shipped, -3.02% insn ALTTP, bit-identical).
- Static recompilation / XIP (-42% insn but 46→3.5 fps on device, I-cache
  stalls; `rc-XIP` is the canonical example of the rig lying).
- N-SPC HLE (38% audio breakage, sealed).
- 4bpp tile-decode cache (M7 barrel shifts are near-free; cache lookup cost
  exceeds the decode it saves).
- PPU dirty-rect (0% hit rate with colour-math guard active).
- Overclock — SD hardware forbids it. 340 MHz is already 21% over the 280 MHz
  datasheet nominal and 353 MHz was unstable for Genesis.

## Status (2026-08-07)

| Lever | State | Evidence |
|-------|-------|----------|
| LINE_CACHE | **shipped** | `testbed-full-20260807-1406`. Device: Zelda 40→51fps. Rig p50=66.5, p90=60fps. Bit-identical (STATEHASH/AUDIOHASH). |
| Mode 7 guard + bypass | **verified + fixed** | User host run: bit-identical on real Mode 7 (SMK lit=56643, 1200f). Bypass fix (`ppu->mode != 7 &&` at call site) eliminates the +18.5% overhead. Rig: ALttP cache preserved, SMK bypassed. |
| Cache-machinery (word-wise RegsMatch + sprite-eval skip) | **shipped** | Rig -5.2% (4,730K→4,484K Lever A alone). Combined A+B: emu p50=4,369K (71.4fps), p90=4,979K (62.7fps). Bit-identical. |
| DSP idle-voice BRR skip | **closed** | Safe version implemented + measured: -0.22% insn, AUDIOHASH identical. The -2.74% ablation ceiling was unreachable without also dropping BRR state advance. |
| Dispatch DMA-check hoist | **shipped** | `run_dots` hoists `dma_active` out of the per-opcode check, refreshed only on actual DMA start. p50=4,304K (-1.5% from 4,369K). Bit-identical. Device gain likely larger (2 eliminated DTCM loads per opcode). |
| Stretcher fill_lp | **shipped** | `snes_audio_stretch.c` retune() now uses EMA(1/8) low-passed fill for corr. Step range 28¢→18¢ (-36%), sharp jumps eliminated. Host-verified; device listening test pending user flash. |
| Floor profiling | **complete** | CPU interpreter = 70% of frame, irreducible. APU 12.6%. Cache 10.1% (already word-wise). Dispatch 7.4% (DMA hoist applied, IRQ hoist neutral → reverted). Levers exhausted. |
| Cache-miss tail | **open, low ceiling** | p99=7.7M (40.3fps projected). The tail is now the only sub-60 band, but narrowing it is a few percent of 10% of frames. |

## What remains

1. **Mode 7 device flash test** (confirmation only). User host run already
   proved bit-identical on real Mode 7 content (SMK lit=56643, 1200f). The
   bypass fix eliminates the +18.5% overhead. Device flash would confirm the
   bypass produces identical timing to pre-cache SMK, but correctness is
   settled.
2. **Cache-miss tail** — the only sub-60 band. p99=7.7M (40.3fps projected)
   on the ~3.4% of lines that miss the cache each frame. The PPU render on
   those lines is irreducible (all known PPU levers are closed). The
   dependency-check overhead on a forced miss is ~1.6K insn/miss-line × ~8
   miss-lines/frame = ~13K insn/frame — not the lever.
3. **Stretcher device listening test** (user-only). `fill_lp` EMA in
   `retune()` cuts step range 28¢→18¢ on the host pattern. Confirms on
   device only by ear — the stretcher is a resampler by design and its
   output is not a bit-identical surface.
4. **SMK 28.3fps gap.** SMK is Mode 7 → cache is bypassed → cache-machinery
   optimisation does not help it. The 5% workload gap for SMK lives in the
   CPU/DMA/IRQ floor (3,587K insn/frame) plus DSP-1 HLE, neither of which
   the PPU cache touches. Separate axis. Floor profiling showed the floor
   is 84% CPU interpreter — irreducible without a different engine (sm/
   zelda3/smw use native C reimplementations, not interpretation).

### Levers tried this session, dead-ended

| Lever | Result | Why |
|-------|--------|-----|
| IRQ-enabled hoist in `run_dots` | +1.9% (worse) | Refresh cost = saved early-return cost. Net zero. |
| Cache-miss memset deferral | STATEHASH changed | Dep-tracking memset timing matters for CPU-phase writes. |
| `PpuLineCacheMatchPpu` (skip Capture on hits) | +0.3% (noise) | 40-branch chain offsets Capture savings in insn count. |
| DSP BRR idle-voice skip | -0.22% | Safe version still advances BRR state; decode loop is cheap. |
| DMA ablation | DMA = 2 calls/frame | Confirmed DMA is negligible — not a lever. |
