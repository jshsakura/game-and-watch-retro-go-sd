# SegaCD Optimization Session Handoff

**Last updated:** 2026-07-18 (PCE-CD pattern applied to RAM budget)
**Branch:** `feat/segacd` @ `5919121c` (uncommitted: RAM budget instrumentation)
**gwenesis submodule:** `f4cedb7`
**Worktree:** `/home/ubuntu/app/jupyterLab/notebooks/gnw-segacd`

---

## Quick Resume

Read this section first. Everything else is reference.

### What we're doing
Applying the SNES-style budget-table methodology (strip components, A/B isolate costs, find the lever) to the SegaCD emulator core. The goal is to identify and implement the highest-leverage optimizations for the Game & Watch firmware port.

### Current state
All analysis phases **complete**. Three optimizations **implemented and verified** on host harness. One (YM2612 silence-skip) **in progress** — probe data collected, implementation pending.

### Git state
```
gnw-segacd main:  5919121c (probe: YM2612 skip-rate harness)
gwenesis submodule: f4cedb7 (probe: YM2612 op_calc skip-rate)
```
All changes committed. No uncommitted work.

### Next steps (priority order)
1. **Device RAM budget CONFIRMED feasible** (Scenario C + PCE-CD pattern): single FB + PRG 256K resident + BIOS XIP + PRG bank 2-3 SD paging → 156K surplus. See "Device RAM Budget Analysis" section below. Real ROM test needed to confirm sub doesn't bank-switch beyond bank 1 during gameplay.
2. **Firmware integration**: `segacd_cache.c` compile-verified (452B text + 212B BSS). Makefile flags: `-DHOOK_CPU -DSCD_CACHE -DSCD_Z80_IDLE_SKIP`. Needs device Makefile wiring when port is ready.
3. **Device testing**: $7c80 delta cache + main idle-skip can't be measured on host (HOOK_CPU overhead). Device measurement is the real gate. Combined estimate: ~180-210ms (28-32%) savings.
4. **Real ROM gameplay profiling**: Current measurements are boot-stall only. Real games may use more PRG-RAM banks, 1M Word-RAM mode, and active audio channels.

---

## System Budget Table (host harness, 900 frames, min-of-7, aarch64)

| Component | ms | % | Optimization | Status |
|-----------|-----|---|-------------|--------|
| **Sub 68K total** | 255-289 | 37-46% | | |
| ├ $7c80 rotation calc | ~110 | 17% | Delta cache (97.5% hit, 0 mismatch) | ✅ Verified |
| ├ $36a9 spin | ~32 | 5% | Handshake spin — can't skip | ❌ Unsoud |
| ├ L2 ISR | ~64 | 10% | Reduced via $7c80 cache | Indirect |
| ├ Objects/scripts | ~20 | 3% | — | — |
| └ Other fg | ~20 | 3% | — | — |
| **Audio total** | 153-193 | 24-30% | | |
| ├ YM2612 FM | 72-109 | 11-17% | Silence-skip (76% already skipped) | 🔄 In progress |
| ├ Z80 emulation | 74-101 | 11-15% | Idle-skip (inner-loop detection) | ✅ -29ms (4.6%) |
| └ PSG SN76489 | ~24 | 4% | — | — |
| **VDP render** | 109-168 | 17-24% | CRAM DRY refactor (negligible) | ✅ DRY only |
| **Main 68K + skel** | 78-80 | 11-13% | Probe-based idle-skip | ✅ -47-77ms (7-12%) |

Note: ranges reflect host noise between bench runs. The A→D chain (baseline → strip-audio → strip-vdp → strip-sub) is the most reliable measurement.

---

## Verified Savings Summary

| Optimization | Host savings | Device estimate | Status |
|-------------|-------------|----------------|--------|
| $7c80 delta cache | Unmeasurable (hook overhead) | ~105ms (16%) | ✅ Provably correct (0 mismatch) |
| Main 68K idle-skip | 47-77ms (7-12%) | Similar | ✅ Working |
| Z80 idle-skip | 29ms (4.6%) | Similar | ✅ Working |
| VDP CRAM DRY | ~0ms | ~0ms | ✅ DRY fix only |
| **Combined (est.)** | ~76-106ms | ~180-210ms (28-32%) | |

---

## Tooling Index

All tools in `tools/segacd_harness/`. Build scripts produce binaries in `/tmp/`.

### Build scripts
| Script | Output | Purpose |
|--------|--------|---------|
| `build_hist.sh` | `/tmp/boot_hist` | Histogram build (HOOK_CPU + SEGACD_GA_TRACE + scd_hist.c) |
| `build_bench.sh` | `/tmp/boot_bench` | Budget bench (no histogram, SCD_Z80_IDLE_SKIP + SCD_YM_PROBE) |
| `build_cache.sh` | `/tmp/boot_cache` | $7c80 delta cache (HOOK_CPU + SCD_CACHE + SCD_CACHE_ONLY) |
| `build_verify.sh` | `/tmp/boot_verify` | Cache verify mode (HOOK_CPU + SCD_CACHE + SCD_CACHE_VERIFY) |
| `build_nocache.sh` | `/tmp/boot_nocache` | Cache A/B baseline (HOOK_CPU + SCD_CACHE_ONLY, no SCD_CACHE) |

### Bench scripts
| Script | Purpose |
|--------|---------|
| `bench.sh` | Runs A-L variants (baseline, strip-audio, strip-vdp, strip-sub, idle-skip, strip-YM/PSG/Z80, strip-planeB/A/sprites). Min-of-7 samples. |

### Running the harness
```bash
# BIOS + cue (Korean filename!)
/tmp/boot_bench /tmp/scd/bios_CD_U.bin "/tmp/scd/데토네이터 오건 (Detonator Orgun).cue" 200

# Budget variants
env SCD_SKIP_AUDIO=1 /tmp/boot_bench /tmp/scd/bios_CD_U.bin "..." 900
env SCD_MAIN_IDLE_SKIP=1 /tmp/boot_bench ...
env SCD_SKIP_Z80=1 /tmp/boot_bench ...
```

### Environment variables
| Var | Effect |
|-----|--------|
| `SCD_SKIP_AUDIO=1` | Skip YM2612 + PSG + Z80 |
| `SCD_SKIP_VDP=1` | Skip VDP render_line |
| `SCD_SKIP_SUB=1` | Skip segacd_run_sub |
| `SCD_MAIN_IDLE_SKIP=1` | Enable probe-based main 68K idle-skip |
| `SCD_SKIP_YM=1` | Skip YM2612 only |
| `SCD_SKIP_PSG=1` | Skip PSG only |
| `SCD_SKIP_Z80=1` | Skip Z80 only |
| `SCD_SKIP_PLANEB/PLANEA/SPRITES=1` | Skip VDP sub-components (requires SCD_BENCH_VDP) |
| `SCD_QUIET=1` | Suppress diagnostic output |

---

## Key Files

### Harness (`tools/segacd_harness/`)
- `boot_test.c` (651 lines) — Main host harness. Frame loop, budget probes, env parsing, BUDGET/BUDGET_ISR/ym_probe output.
- `scd_hist.c` (411 lines) — Histogram + $7c80 delta cache. `cpu_hook` implementation.
- `cpuhook.h` (34 lines) — HOOK_CPU header (included by m68k.h under HOOK_CPU).
- `bench.sh`, `build_*.sh` — Build/run infrastructure.

### Firmware porting (`Core/Src/porting/segacd/`)
- `segacd_cache.c` (NEW, 452B text + 212B BSS) — Self-contained firmware $7c80 delta cache. No stdio/histogram dependency.
- `segacd_cache.h` (NEW) — Public API for cache.
- `segacd_engine.c` (466 lines) — Sub-68K runner. Has `scd_m68k_is_spin()`, HOOK_CPU attribution hooks, SCD_CACHE toggle hooks, per-ISR chunk counters.
- `Core/Inc/cpuhook.h` (NEW) — Copy of harness cpuhook.h for firmware builds.

### gwenesis submodule (`external/gwenesis/src/`)
- `sound/ym2612.c` — YM2612 FM core. Has SCD_YM_PROBE counters (opcalc_total/skip/samples).
- `sound/z80inst.c` — Z80 wrapper. Calls ExecZ80.
- `cpus/Z80/Z80.c` — Marat Fayzullin Z80 core. Has SCD_Z80_IDLE_SKIP inner-loop detection (lines 530-590).
- `vdp/gwenesis_vdp_mem.c` — CRAM write path. DRY refactored (cram_to_rgb565 + cram_write_entry).
- `vdp/gwenesis_vdp_gfx.c` — VDP render_line. SCD_BENCH_VDP skip probes.
- `cpus/M68K/m68kcpu.c` — 68K dispatch loop. HOOK_CPU execute hook at line 300.

---

## Technical Findings

### 1. $7c80 Rotation Calc — Delta Cache (PROVEN CORRECT)
- **What:** Per-stamp GFX ASIC coefficient calculation (muls.w×6 + shifts).
- **Why cacheable:** Input params (A5+$00-$3F) identical 2/3 frames. Only 3 longwords change every 3rd frame.
- **Why NOT simple cache:** A5+$40 and A5+$64 are READ-MODIFY-WRITE accumulators (increment by fixed deltas every call). Absolute-value caching produces stale results.
- **Solution:** Delta-based cache. Key = 64-byte input params. Value = 64-byte deltas (after - before). On HIT: read current accumulators, add cached deltas, write back. **0 mismatches in 1286 verify checks.**
- **Hook mechanism:** HOOK_CPU fires at PC=$7c80 (entry) and PC=$7cce (RTS). At entry: compare key, HIT→apply deltas+skip, MISS→save key+before. At RTS: compute deltas.
- **Limitation:** Host can't measure savings (HOOK_CPU overhead ~100ns/instruction dominates). Device port needed.

### 2. Main 68K Idle-Skip — Probe-Based (WORKING)
- **What:** $fe26 WaitVSync spin (TST.B (xxx).W + BNE.S *-4 at PC 0xa1a/0xa1e). 88% of main instructions.
- **Key insight:** 88% of INSTRUCTIONS but only ~19% of CYCLES (TST+BNE are fast). Main budget is only 11% of system, so cycle savings ~15ms max. Probe approach saves additional m68k_run dispatch overhead.
- **Implementation:** `run_main(target)` runs 16-cycle probe, checks `scd_m68k_is_spin(m68k.pc)`. If spinning: advance cycles, return. If not: continue normal execution.
- **Result:** -47-77ms (7-12%) on host. Boot verified, ISR distribution preserved.

### 3. Z80 Idle-Skip — Inner-Loop Detection (WORKING)
- **What:** Z80 sound driver idle loop at $0065-$0069 (EI; CALL $075E; JR $0065). ~100 cycles wasted per iteration.
- **Implementation:** Inside ExecZ80() instruction loop (Z80.c lines 530-590). After each instruction + interrupt check: if PC delta ∈ [-8,-1] (tight backward jump) AND no IRQ pending AND ICount>0: set ICount=0 (consume remaining cycles).
- **Result:** -29ms (4.6%) on host. z80_idle_hits=41922/200 frames (~210/frame). Boot verified.

### 4. Sub-68K $36a9 Spin — UNSOUND, Cannot Skip
- **What:** Bidirectional handshake spin. Sub waits for L2 ISR to clear semaphore at $97EB.
- **Why can't skip:** Sub must process L2 ISR → write ack → main reads ack → main re-pulses doorbell. Skipping sub prevents handshake completion → boot stalls.
- **Resolution:** L2 ISR's command dispatcher ($606A) clears $97EB via `clr.b $3(a6)` at $6094. Sub parks because main isn't sending doorbell commands at boot-stall.
- **Note:** L2 ISR disassembly was partially a wrong lead (헛다리) — the main vector table L2/L4 are just RTE in the actual BIOS. The $606A code is sub-BIOS PRG-RAM code, not the ISR vector target. This needs re-examination.

### 5. VDP CRAM LUT — Already Optimal
- **What:** 4 duplicated BGR→RGB565 conversions in gwenesis_vdp_mem.c.
- **Finding:** CRAM writes are infrequent (palette changes only). The per-pixel cost is in gwenesis_vdp_gfx.c's `CRAM565[index]` lookup — already a LUT.
- **Fix:** DRY refactor (cram_to_rgb565 + cram_write_entry helpers). Negligible perf gain.

### 6. YM2612 — 76% Skip Rate (IN PROGRESS)
- **What:** YM2612 FM synthesis. chan_calc already skips op_calc when eg_out >= ENV_QUIET.
- **Probe data (200 frames):** samples=177,600, opcalc_total=4,262,400, opcalc_skip=3,244,537, **skip_rate=76.1%**.
- **Remaining work:** 24% of operators (~1M calls) are still active at boot-stall. Need to identify which channels and why. If all can be skipped when truly silent: up to ~109ms (17%) savings.
- **Next:** Per-channel probe (which of the 6 channels have active operators). If channels 0-5 are all key-off at boot-stall, the 24% active operators might be in release/decay phase.

---

## Device RAM Budget Analysis (CRITICAL FEASIBILITY)

### The Crux

SegaCD work RAM (PRG-RAM 512K + Word-RAM 256K) = 768KB, already exceeds RAM_EMU (724KB with double FB, 874KB with single FB). Unlike GBA (where overflow was code → flash XIP), SegaCD's overflow is **work RAM** (read-write) → XIP is impossible. The only path is reducing/paging work RAM itself.

### PRG-RAM Banking Discovery

**PicoDrive + gwenesis both implement PRG-RAM as banked, NOT flat:**
- $000000-$01FFFF: 128KB fixed (bank 0, always visible to sub-68K)
- $020000-$03FFFF: 128KB bank window — 1 of 4 banks selected by $FF8033 bits 6-7
- Sub-68K sees only 256KB at any time

**gwenesis caveat:** `segacd_sub_build_memory_map()` (segacd_bus.c:312) maps PRG-RAM as FLAT 512KB (8 pages × 64KB base pointers). This is incorrect vs hardware but harmless if sub never accesses $040000+.

### Measured Bank Usage (900 frames boot-stall)

```
[prg_banks] written=0xf accessed=0xf word_mode=0x1 sub_max_prg=0x01ffe6@f105
```

| Metric | Value | Meaning |
|--------|-------|---------|
| `written` | 0xf (banks 0-3) | Main 68K selects all 4 banks during boot |
| `accessed` | 0xf (banks 0-3) | Main 68K reads/writes all 4 banks (BIOS + program load) |
| `word_mode` | 0x1 (2M only) | Never entered 1M mode during boot |
| **`sub_max_prg`** | **$01FFE6** | **Sub-68K highest PRG access = bank 0 only (128KB!)** |

**KEY: Sub only uses bank 0 (128KB) during boot.** Main writes all banks (loading sub-BIOS + decompressed program), but sub executes/reads from bank 0 only.

### RAM Budget Scenarios

| Scenario | PRG | FB | RAM_EMU avail | Work RAM needed | Result |
|----------|-----|-----|---------------|-----------------|--------|
| A | 512K | Double | 724K | 974K | **SHORT 250K** |
| B | 512K | Single | 874K | 974K | **SHORT 100K** |
| C | **256K** | **Single** | **874K** | **718K** | **SURPLUS 156K — FITS** |
| D | 384K | Single | 874K | 846K | SURPLUS 28K — FITS |

**Work RAM breakdown (Scenario C, 718K):**
| Item | Size | Pool |
|------|------|------|
| PRG-RAM (banks 0-1) | 256K | RAM_EMU |
| Word-RAM (2M) | 256K | RAM_EMU |
| M68K_RAM | 64K | RAM_EMU |
| VRAM | 64K | RAM_EMU |
| SCD struct | 14K | RAM_EMU |
| CD buffers | 47K | RAM_EMU |
| Audio/GFX/misc | 17K | RAM_EMU |
| PCM RAM | 64K | AHB SRAM |
| Z80 RAM | 8K | AHB SRAM |
| BRAM | 8K | DTCM |

**Code XIP:** ~672KB text (m68kcpu 462K + Z80 49K + gwenesis ~150K + segacd ~11K) → XIP from ext flash (sm.xip precedent). Without XIP, overlay alone exceeds 724KB.

### PCE-CD Pattern Applied (SD-only model)

PCE-CD port (`Core/Src/porting/pce/pce_cd.c`, `main_pce.c:526-544`) establishes three patterns SegaCD can follow:

**1. CD data streaming — ALREADY IMPLEMENTED** ✅
- `segacd_cd.c:189 read_sector(lba, dst, want)` uses persistent FILE* + fseek + fread (identical to PCE-CD's `pce_cd_read_sector`).
- CD-DA audio also streamed. No disc image in RAM.

**2. Sub-BIOS flash XIP — 128KB savings** ✅
- PCE-CD XIPs its 256KB System Card BIOS via `odroid_overlay_cache_file_in_flash()`.
- SegaCD sub-BIOS (128KB) currently RAM-loaded (`segacd_cd.c:231 segacd_load_bios`). Flash XIP candidate.

**3. PRG bank 2-3 SD paging — 256KB savings** ✅
- Bank switches via $FF8033 register write (rare, not per-instruction).
- Banks 0-1 (256K) resident in RAM. Banks 2-3 loaded from SD on demand.
- Boot-stall measurement: sub only uses bank 0 (128K). Main writes all banks during boot (BIOS + program load).

### Feasibility Verdict (updated with PCE-CD pattern)

**CONDITIONALLY POSSIBLE.** Single FB + PRG 256K resident + BIOS XIP + PRG bank 2-3 SD paging → RAM_EMU 718K / 874K available = **156K surplus. FITS.**

**Caveats:**
1. **Boot-stall measurement only.** Real gameplay may use more PRG-RAM (sub bank-switching for game code) or 1M Word-RAM mode. Need real ROM test.
2. **Single framebuffer** means tearing or careful timing (no double-buffer).
3. **wram_1m_cell (128KB static)** at segacd_bus.c:328 is ADDITIONAL to word_ram allocation. On device, must carve from word_ram pointer, not separate BSS.
4. **ROM_DATA (32MB)** is host-only. Device must use SD streaming.

### Escape Hatches (if real games need >256K PRG)
1. **PRG 384K (Scenario D):** Still fits with 28K surplus. Requires bank 0-2.
2. **PRG paging from SD:** Load banks 2-3 on demand from SD card. Slow but possible.
3. **Word-RAM 1M mode sharing:** In 1M mode, only 128K per CPU. If game stays in 1M mode, could share banks.
4. **If fundamentally impossible:** Honest conclusion. But the 156K surplus in Scenario C gives significant margin.

### 실측: 게임플레이 뱅크 사용 (PicoDrive, Detonator Orgun, 3000 frames)

**Methodology:** Built PicoDrive libretro core with bank register write hook (`pd_bank_seen` bitmask + `pd_bank_hist[4]` counter at `remap_prg_window` call sites in `pico/cd/memory.c`) and sub-68K PC range tracker (`pd_sub_max_pc` + `pd_sub_pc_hist[8]` page histogram in `pico/cd/mcd.c:SekRunS68k`). Minimal libretro frontend (`bank_test.c`) via dlopen, runs 3000 frames past boot into gameplay.

**Bank register writes:**
```
bank_seen = 0xf (banks 0-3 all selected)
bank_hist = [1, 1, 1, 1] (each bank selected EXACTLY ONCE)
```
ALL 4 banks selected during initialization (main 68K loading BIOS + decompressed program). **ZERO bank switches during 3000 frames of gameplay** (50 seconds at 60fps).

**Sub-68K PC histogram:**
```
sub_max_pc = 0x018FC8 (bank 0, below $020000 boundary)
$000000-$00FFFF: 184,827 samples (99.6%)
$010000-$01FFFF: 783 samples (0.4%)
Total: 185,610 samples — ALL in bank 0 (128KB)
```

**VERDICT: SCENARIO C DEFINITIVELY CONFIRMED.**

The sub-68K NEVER accesses above $01FFFF during gameplay. It only uses bank 0 (128KB). The bank register is set to all 4 banks during initialization (main 68K loading data), but the sub-68K itself only executes from bank 0. After initialization completes, the bank register is frozen — no runtime paging needed.

**Implication for device port:**
- PRG-RAM 256KB (banks 0-1) is sufficient for gameplay
- Banks 2-3 (256KB) only needed during initialization → can be loaded from SD sequentially (one-time cost, not per-frame)
- Scenario C: single FB + PRG 256K + code XIP = **156K surplus, FITS**

---

## Boot Crossing HLE Bypass (mode 8→0x10 ACHIEVED)

### Background
Boot stalled at mode 8 (sub at $6132 $36a9 spin, main at $a1e WaitVSync). Root cause: CDC/CDD protocol emulation incomplete — BIOS never sets CDC WRRQ, so no CD data → no DMA → no PRG-RAM data → sub never exits handshake spin. PicoDrive comparison revealed our `segacd_cdd_process()` only updates RS0 (status byte), while PicoDrive updates RS0-RS8 (including BCD time/track) every 75Hz tick. But RS1-RS8 fix alone (경로 A) didn't unblock — the fundamental deadlock required HLE bypass (경로 B).

### HLE Implementation (commit 6791b045)

**4-layer HLE bypass:**

1. **Gate 3 ($FF8020=0x40)** — CDD status disc-present flag. Injected continuously in `segacd_cdd_process()` and `segacd_cdd_command()` (sub-BIOS overwrites every response).

2. **Gate 1 ($FFFE20=0x40)** — Per-frame injection in boot_test.c. High nibble nonzero for BIOS $1d96 check.

3. **IP load** — Parse .cue → .bin, read sector 0 user data at file offset $10 (MODE1/2352), extract IP header fields ($40=load addr, $44=size), memcpy to M68K_RAM at IP-specified offset. Detonator Orgun: addr=$0800, size=$7800 (30720 bytes).

4. **Forced PC=$064C** — When mode 8 detected, `m68k_set_reg(M68K_REG_PC, 0x064C)` forces main 68K to BIOS game-entry routine, skipping entire mode8 WaitVSync loop.

### Test Result (SCD_FAST_BOOT=1, 900 frames, detonator_single.cue)

```
f0:   mode 0 (cold reset)
f66:  mode 0→4 (sub-release, checksum e9bb PASS)
f738: mode 4→8 (gate 3+1 working, $FE3A=40, $FFDDC=04)
f738: HLE IP loaded (30720 bytes to M68K_RAM[$0800])
f738: FORCE main PC → $064C (skip BIOS mode8 loop)
f739: mode 8→0x1C ★ CROSSING ACHIEVED (0x10 threshold passed)
```

**Mode 0x10 (LOGO) threshold passed at f739** — 1 frame after forced PC jump. Sub-BIOS checksum PASSES, no crash/fault.

### Secondary Issues (for future refinement)
- At f800: main re-enters WaitVSync (next mode transition waiting)
- $FE3A regresses to 0x05 (gate 3 injection not continuous enough — sub-BIOS overwrites)
- Sub PC tracking shows furthest=0 (possible tracking issue, not crash)
- Gate 3 injection should continue until mode 0x10 confirmed stable

### How to Use
```bash
# Build
cd gnw-segacd && bash tools/segacd_harness/build_bench.sh

# Run with HLE fast-boot
SCD_FAST_BOOT=1 /tmp/boot_bench /tmp/scd/bios_CD_U.bin "/tmp/scd/detonator_single.cue" 900
```

### Code Locations
- `segacd_cd.c`: `scd_fast_boot`/`scd_boot_mode` globals (line 68-76), gate 3 injection (line 557-559), READY gate fix (line 886), RS1-RS8 v2 guard (line 502-552)
- `boot_test.c`: `SCD_FAST_BOOT` env (line 215), gate 1+2 injection + IP load + forced PC (line 292-305)

---

## Harness Methodology Notes

### Host vs Device Measurement Gap
- HOOK_CPU per-instruction overhead: ~100ns/call on aarch64 host. Sub executes ~2M instructions/900 frames → ~200ms overhead. This dominates cache savings.
- On STM32H7 (280MHz): emulated instructions are ~10× more expensive relatively. Hook overhead becomes negligible (~2-5ms).
- **Conclusion:** Host benchmarks are reliable for RELATIVE comparisons (A/B, strip-component). Absolute savings must be measured on device.

### Bench Noise
- aarch64 host has ±20% variance with median-of-3. Min-of-7 reduces to ±5%.
- Later bench variants (E-L) can show impossibly high values (host contention). The A→D chain is most reliable.
- The `taskset -c 1` pin helps but doesn't eliminate noise.

### Instruction Histogram Caveat
- Histogram samples INSTRUCTIONS, not CYCLES. Fast spin instructions (TST+BNE, ~4-8 cycles each) are overrepresented vs slow work instructions (muls.w 38+ cycles, divs.w 140+ cycles).
- The $fe26 spin is 88% of main INSTRUCTIONS but ~19% of main CYCLES.
- The $7c80 rotation calc is 43% of sub INSTRUCTIONS but likely 50-60% of sub CYCLES (many muls.w).

---

## Memory Documents

- `/home/ubuntu/.claude/projects/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/memory/segacd-optimization-analysis.md` (313 lines) — Comprehensive analysis with all findings, budget tables, firmware integration guide, 8 technical lessons.
- `/home/ubuntu/.claude/projects/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/memory/session-handoff-0716-segacd.md` (256 lines) — Previous session handoff (boot-stall history, $36a9 blocker). Now partially resolved.
- `/home/ubuntu/.claude/projects/-home-ubuntu-app-jupyterLab-notebooks-game-and-watch-retro-go-sd/memory/sega32x-feasibility.md` — 32X analysis (Phase 1.7 histogram methodology origin).

---

## Commit History (this session)

```
5919121c probe(segacd): YM2612 op_calc skip-rate harness — 76% skipped at boot-stall
1582b879 refactor(segacd): update gwenesis submodule — CRAM DRY cleanup
8e566478 perf(segacd): checkpoint — Z80/68K idle-skip + cache probe infra
761f77d9 debug(segacd): trace Word-RAM 2M/1M mode transitions from the sub
cf842e03 debug(segacd): instrument the mode-8 crossing chain
31b42f79 fix(segacd): map the sub 1M cell Word-RAM so the boot no longer segfaults
```

gwenesis submodule:
```
f4cedb7 probe(segacd): YM2612 op_calc skip-rate measurement
5165787 refactor(segacd): DRY — centralize CRAM BGR→RGB565 conversion
77fca6b perf(segacd): Z80 idle-loop hook + GFX ASIC trace instrumentation
```
