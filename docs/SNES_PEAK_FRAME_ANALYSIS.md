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
work is in two places: (1) the 10% cache-miss tail (p99=8.0M, 38.9 fps projected
— these are the frames that dip below 60 on device), and (2) the untouched 4.14M
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
| DSP idle-voice BRR skip | **closed** | Safe version implemented + measured: -0.22% insn, AUDIOHASH identical. The -2.74% ablation ceiling was unreachable without also dropping BRR state advance. |
| Cache-miss tail | **open, low ceiling** | p99=8.0M (38.9fps projected). 7.3% slower than baseline max due to dependency-check overhead on forced full re-render. Narrowing it is a few percent of 10% of frames. |

## What remains

1. **Mode 7 device flash test** (confirmation only). User host run already
   proved bit-identical on real Mode 7 content (SMK lit=56643, 1200f). The
   bypass fix eliminates the +18.5% overhead. Device flash would confirm the
   bypass produces identical timing to pre-cache SMK, but correctness is
   settled.
2. **Cache-miss tail investigation** (optional). If the 10% tail's
   dependency-check overhead on forced full re-render can be made cheaper (e.g.
   skip the check when VRAM page serial shows >N pages dirtied), the tail
   narrows. Low ceiling — a few percent of already-10% of frames.
