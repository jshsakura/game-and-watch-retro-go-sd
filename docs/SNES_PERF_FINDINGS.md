# SNES Performance Optimization Campaign — Full Report

## Summary

This is a comprehensive record of the SNES 60fps optimization campaign on the
Game & Watch (STM32H7B0VB Cortex-M7 @ 480MHz, SD-card variant). Every lever
tried — what worked, what didn't, and why — is documented here so future work
targets the real cost centers, not the ones that look expensive on paper.

**Two optimizations shipped:**

| Optimization | Rig insn Δ | Device fps | Status |
|---|---|---|---|
| Spin-skip ROM whitelist + cheapen hook | SMW −1.14%, Zelda +16% avoided | 37.7 → **46 fps** | ✅ committed, in release |
| rc-SMW native recompilation (per-ROM) | **−42.3%** (4.63M vs 7.92M insn/frame) | *awaiting measurement* | ✅ committed, `RCSMW=1` gate |

**Three optimizations tried and reverted** (DSP echo skip, PPU tile-cache,
PPU dirty-rect) — all dead for the same root cause documented below.

---

## Measurement Environment

- **Device**: STM32H7B0VB Cortex-M7 @ 480MHz, SD-card variant
- **Rig**: QEMU mps2-an500 `-icount shift=0` (`tools/m7_qemu_rig/`)
  - ARMv7-M instruction counts are exact
  - **Cache, bus, and wait-state costs are invisible** — the rig gives
    algorithmic cost decomposition, not absolute device fps
- **Gate**: STATEHASH + AUDIOHASH must be bit-identical on both SMW and Zelda.
  Any divergence = audible/visible change requiring explicit approval.
- **Baselines** (clean tree, 120 frames, window=40):

| ROM | STATEHASH | AUDIOHASH | avg emu insn/frame | avg apu insn/frame |
|-----|-----------|-----------|---------------------|---------------------|
| SMW (`external/smw/smw.sfc`, 512KB) | `17012c30` | `b2cc73eb` | 5,241,065 | 253,820 |
| Zelda ALttP (`roms/zelda_alttp.smc`, 1MB) | `afc14c92` | `106cf133` | 4,539,752 | 174,444 |

Reproduce: `RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes_hf.sh <rom> 120`

---

## Device Measurement — Zelda ALttP (generic core, spin-skip OFF)

Profile data captured on hardware (profile2, auto-OC to 312 MHz):

| Field | Value | Meaning |
|-------|-------|---------|
| `clk` | 312,000,000 | Core clock (profile2 auto-OC) |
| `emu_skip` | 6,446,520 cyc | Emulation cycles per frame |
| `emu_draw` | 6,970,520 cyc | Emulation + draw cycles per frame |
| `audio` | 615,009 cyc | Audio synthesis cycles per frame |
| `spin` | 0%, gate=0 | Whitelist OFF for Zelda (as designed) |
| **Measured fps** | **37.9** | CPU 99% |

### Per-frame budget decomposition

| Component | cyc/frame | Source |
|-----------|-----------|--------|
| Emulation (65816 + PPU + DMA) | 6,446,520 | `emu_skip` |
| Draw (LCD render) | 524,000 | `emu_draw − emu_skip` |
| Audio (SPC700 synthesis) | 615,009 | `audio` |
| LCD blit + vsync + OS (outside profile) | ~644,800 | `312M/37.9 − (emu_draw + audio)` |
| **Total** | **~8,230,300** | `312M / 37.9 fps` |

The ~3 fps gap between theoretical (312M / 7.59M = 41.1 fps) and actual
(37.9 fps) is the LCD blit / vsync / scheduler overhead not captured by
the profile counters.

### Rig → Device CPI conversion

The M7 rig (QEMU icount) gives exact ARMv7-M instruction counts but cannot
model cache misses, bus contention, or wait states. The device cycle count
divided by the rig instruction count yields the effective CPI (cycles per
instruction), which quantifies the memory-system overhead:

| Metric | Rig (insn/frame) | Device (cyc/frame) | CPI |
|--------|-------------------|---------------------|-----|
| Emulation only | 6,264,539 (Zelda 1200f hf) | 6,446,520 | **1.029** |
| Emulation + audio | 6,438,983 | 7,061,529 | 1.097 |
| Total frame | 6,438,983 | 8,230,300 | **1.278** |

**Emulation CPI ≈ 1.03** — the device's cache system runs the 65816/PPU
emulation path at nearly exactly 1 cycle per ARM instruction. The rig's
instruction count is a highly accurate predictor of device emulation cost
(within 3%). The remaining 0.25 CPI (total frame) comes from draw, LCD
blit, and vsync overhead that the rig does not model.

This CPI factor can be used to predict device performance from rig data:
`device_cyc ≈ rig_insn × 1.03 (emu) + draw/audio/blit overhead`.

---

## Shipped Optimization #1: Spin-Skip ROM Whitelist

### Background

The SNES CPU spends significant time in NMI-wait spin loops
(`LDA flag / BEQ spin`) between frames. These iterations are semantic no-ops:
registers are bit-identical each pass, no writes, no IO reads — only the NMI
handler can change the polled byte. The spin-skip learner
(`external/sm/src/snes/spin_skip.c`) detects these loops and replays them in
bulk without running the interpreter.

### Problem: ROM-dependent cost-benefit

The per-memory-access hook overhead (`spin_hook_read`/`spin_hook_write` in
`cpu.c`, called on every `cpu_read`/`cpu_write`) exceeds the replay savings
for low-spin ROMs. Measured breakeven ≈ 45–50% skip rate:

| ROM | Gameplay skip% | Spin OFF (hf) | Spin ON (cheapened) | Δ | Verdict |
|-----|----------------|---------------|---------------------|---|---------|
| SMW | 56.6% | 7,845,222 | 7,755,817 | **−1.14%** | ✅ win |
| Zelda ALttP | 25.0% | 6,264,539 | 7,284,909 | **+16.3%** | ❌ regression |

### Fix 1: Cheapen the per-access hook

Gated the cross-TU `spin_hook_read`/`spin_hook_write` calls behind
`if (g_spin.phase)` at the caller in `cpu.c:58-66`. When the learner is idle
(phase==0, the common case), the BL+BX+prologue (~4 insn) is skipped entirely.
Bit-identical behavior — only the call overhead is removed.

### Fix 2: ROM whitelist (replaces runtime auto-detection)

A runtime profitability gate was tried (disable spin-skip if skip% < 40% after
a 600-frame observation window) but proved **unreliable**: title-screen skip%
does not predict gameplay skip%. SMW goes 31% (title) → 57% (gameplay); an
instant A/B at boot would wrongly disable spin-skip for a ROM that benefits
from it.

Replaced with a build-time pre-analyzed table keyed on FNV-1a hash of the
21-byte SNES internal title (offset 0x7FC0 for LoROM, 0xFFC0 for HiROM):

```c
typedef struct { uint32_t hash; bool enable; const char *name; } spin_entry_t;
static const spin_entry_t spin_table[] = {
  { 0xFB0BD0ECu, true,  "SUPER MARIOWORLD  (skip% 56.6% — ON)"  },
  { 0x9C75F6EEu, false, "THE LEGEND OF ZELDA  (skip% 25.0% — OFF)" },
};
```

Default OFF — unregistered ROMs get no spin-skip (safe; the hook is
net-negative for them). New ROMs are added after measuring their 1200-frame
gameplay skip% via `run_snes_spin.sh`.

### Results

- **Device**: 37.7 → **46 fps** (user-measured, SMW)
- **Correctness**: STATEHASH + AUDIOHASH bit-identical both ROMs
- **Commits**: submodule `23cde9d` + `283d3a7`; top-level `e769cb77` +
  `0533ac9f`

---

## Shipped Optimization #2: rc-SMW Native Recompilation

### Background

The rc static translator (`tools/sfc_recomp/`) translates each 65816 opcode
site into a C function at build time. At runtime, a dispatch table maps
PC → native site function, replacing the interpreter loop entirely. SMW
generates 8,371 site functions.

This is a **per-ROM** optimization (each ROM produces different C code). It is
not a generic core improvement — it's a targeted escalation for SMW
specifically, following the project policy: *"push generic levers to max first;
ROM-specific is escalation, not surrender."*

### Dispatch design

The host PoC uses a flat 32MB PC→site map (`calloc(1<<24, sizeof(uint16_t))`).
This cannot fit the device (RAM_EMU = 724KB, DTCM = 128KB). Three device-feasible
dispatch variants were measured:

| Variant | emu insn/frame | vs interpreter | DTCM fit | Notes |
|---------|----------------|----------------|----------|-------|
| Flat banked (upper bound) | 4,379,536 | −44.7% | ❌ 896KB | Rig-only reference |
| **Hash LF~0.5 (chosen)** | **4,565,161** | **−42.3%** | **✅ ~60KB** | Knuth mult, linear probe |
| Binary search | 6,133,507 | −22.6% | ✅ ~33KB | Branch mispredicts cost +1.75M |
| Interpreter baseline | 7,918,806 | — | — | |

Hash dispatch recovers 96% of flat's win while fitting DTCM. Per-bank Knuth
multiplicative hash `(pc * 2654435761u) & mask`, linear probing at LF~0.5,
~6-8 cycles/lookup deterministic.

### Device integration

- **rc_smw.xip** (368,224 bytes = 8,371 sites × ~44 bytes) shipped on SD at
  `roms/homebrew/rc_smw.xip`
- At launch: CRC32 detects SMW (0xB19ED489) → caches rc_smw.xip into external
  flash via `odroid_overlay_cache_file_in_flash_relocate` (same mechanism as
  SM/Zelda3 XIP) → sentinel-patches function pointers → builds hash dispatch
  in DTCM → sets `g_rc_active = true`
- **cpu_runOpcode fast path**: `if (g_rc_active) { id = lookup(k,pc);
  if (id) { call(id); return; } }` — one never-taken branch when inactive
- rc takes priority over spin-skip for SMW (rc −42.3% >> spin-skip −1.14%)
- **RCSMW=1** Makefile gate (default 0). RCSMW=0 = byte-identical to canonical
  release (intflash 261,660 bytes)

### Correctness

- STATEHASH bit-identical to interpreter: `17012c30` (SMW 120f)
- Coverage: 99.99% native (1,722,113 native / 130 interpreter opcodes in 120f)
- Cross-overlay symbol check: OK (33 cores, no aliases)

### Build

```bash
CONTAINER_NAME=retro-go-sd-snesrc make release DOCKER=1 RCSMW=1 \
  COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
  INTFLASH_BANK=2 CHEAT_CODES=1 ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1
```

### Status

- ✅ Committed: submodule `6718ad7` (rc_dispatch + cpu.c), top-level `1c6cd6e3`
  (16 files: linker, Makefile, main_snes.c, rc_smw_sites.c, redefines, generated/)
- ✅ Included in release `testbed-full-20260718-2337`
- ⏳ **Device fps measurement pending** — rig says −42.3% insn, but device has
  cache/bus overhead (XIP execution from external flash via QSPI) the rig
  cannot model

---

## Dead Levers (tried and reverted — do not revisit)

All three share the same failure mode: **the Cortex-M7's single-cycle
shift/MAC + cache-resident data make textbook cost centers near-free.**
Micro-optimizing existing loops (cache/LUT-squeeze/hash-skip) adds overhead
that exceeds the savings.

### DSP echo fast-path (`dsp.c`)

**Hypothesis**: Skip the 8-tap FIR MAC when `echoVolL==0 && echoVolR==0 &&
feedback==0` → ~80 insn/sample saved.

**Result**: bit-identical ✅ but savings ~0 (apu 253,820 → 254,509, +0.27%).
Two causes:

1. SMW title music has **no echoEnable channels** → echo is audibly inert
   (FIR multiplies zeros). Stubbing the *entire* echo path produced identical
   AUDIOHASH.
2. M7's single-cycle MAC makes the FIR loop near-free.

**DSP total cost**: apu = 253,820 insn/frame = 4.6% of emu. Echo total =
**0.35% of frame**. Real DSP cost = BRR decode + gaussian interpolation (exact
math; released-silent voice skip already shipped).

### PPU 4bpp tile-cache (`ppu.c`)

**Hypothesis**: Cache decoded 8-pixel color indices by VRAM address → skip
READ_BITS + pixel extraction (~64 ops/tile) → ~10% saved.

**Result**: bit-identical ✅ (82–90% hit rate) but **net regression** (SMW
+0.06%, Zelda +1.6%). On the M7, READ_BITS (2 cache-resident halfword loads,
~2-4 cycles) + pixel extraction (register shifts, 1 cycle each) are near-free.
The cache's scattered byte loads from `tc_pix[]` (2+ cycles each) are **more
expensive** than the register shifts they replace. The real PPU cost is the
z-buffer compare/store loop, which no decode cache can eliminate.

### PPU dirty-rect / frame-delta (`ppu.c`)

**Hypothesis**: Skip the output loop when `bgBuffers[0]` hasn't changed since
the previous scanline (static screens, menus).

**Result**: 120f gate PASSED both ROMs (Zelda 94.7% hit rate, −1.74%). 1200f:
Zelda STATEHASH **diverged**. Root cause chain:

1. XOR hash collision (linear hash cancels when two pixels change by the same
   difference) → fixed with FNV-1a (multiply inside loop breaks linearity)
2. Still diverged → root cause: hash covered only `bgBuffers[0]` (main screen),
   but the output loop's **color math path** also reads `bgBuffers[1]`
   (subscreen). Skipping a math-enabled line when only the subscreen changed
   produces wrong pixels.
3. Adding `math_enabled == 0` guard fixed correctness → hit rate dropped to
   **0%** — both SMW and Zelda use color math on ALL gameplay lines.

The dirty-rect gives zero benefit with correct guarding and adds +0.6% hash
overhead.

---

## PPU Cost Breakdown (measure-first stubs)

Method: insert `return;` at function entry → insn/frame delta = function cost.
(STATEHASH diverges — expected; measuring cost, not correctness.)

| Component | insn/frame | % emu | Method |
|-----------|-----------|-------|--------|
| **Output loop** (fast path: palette565 LUT + 2-pixel word-store) | **~815K** | **15.6%** | stub1 − stub2 |
| BG tile decode (PpuDrawBackgrounds) | 98K | 1.9% | stub 2 |
| Sprite tile decode (ppu_evaluateSprites) | 57K | 1.1% | stub 3 |
| ClearBackdrop + cwin + palette | ~33K | 0.6% | residual |
| **Total PPU** | **~1,003K** | **~19.2%** | |

Key findings:
- Output loop is the dominant PPU cost (~9× all decode combined).
- **Math path never fires on SMW** — forcing fast-path-only produced
  bit-identical STATEHASH. The 815K is entirely:
  `dst = palette565[src & 0xff]` (precomputed cgram × brightness LUT,
  2-pixel word-paired store).
- `PPU_RGB565` defaults to 1 → rig uses the device path → numbers reflect device.
- **All generic PPU levers exhausted**: tile-cache dead, dirty-rect dead,
  output-loop already LUT-optimized. DMA2D offload deferred (architectural
  change, future ticket).

---

## SNES ROM Survey (2,280 titles)

Survey file: `/tmp/snes_survey_full.tsv` (478KB). Columns: ROM filename,
status, sound driver code size (bytes), detected driver patterns, NSPC
analysis.

### Boot compatibility

| Status | Count | % |
|--------|-------|---|
| OK (boots successfully) | 2,097 | **92.0%** |
| BOOT_CRASH | 183 | 8.0% |

### Sound driver distribution

| Driver family | ROM count | % of OK | Notes |
|---------------|-----------|---------|-------|
| **Akao** | 835 | 39.8% | Square, many JRPGs — largest family |
| **Nintendo** | 443 | 21.1% | SMW / Yoshi's Island engine |
| **Konami** | 233 | 11.1% | |
| **Capcom** | 93 | 4.4% | |
| Mori | 34 | 1.6% | |
| Hudson | 22 | 1.0% | |
| Soft Creation | 16 | 0.8% | |
| Others (Compile, Prism, Neverland, Namco, etc.) | 41 | 2.0% | |
| *No driver detected* | 424 | 20.2% | |

Top 2 engines (Akao + Nintendo) cover **1,278 / 2,097 = 61%** of bootable ROMs.

### NSPC engine version

| Version | ROM count |
|---------|-----------|
| v=std | 721 |
| v=SMW | 630 |
| v=GD3 | 14 |
| v=YI | 4 |

### Spin distribution

Not yet measured. Would require running `run_snes_spin.sh` on each of the 2,097
bootable ROMs. The spin-skip whitelist currently registers only SMW (ON) and
Zelda (OFF); expanding coverage requires per-ROM 1200-frame skip% measurement.

---

## Files Modified (cumulative across all shipped work)

### `external/sm/` submodule

| File | Change | Commit |
|------|--------|--------|
| `src/snes/spin_skip.c` | Whitelist table + `spin_whitelist_set()` + profitability gate reverted | `23cde9d`, `283d3a7` |
| `src/snes/spin_skip.h` | `g_spin_whitelist` extern + `spin_whitelist_set` decl, `win_real_snap` removed | `23cde9d` |
| `src/snes/cpu.c` | Cheapened per-access hook (`if (g_spin.phase)`) + rc fast path (`if (g_rc_active)`) | `23cde9d`, `6718ad7` |
| `src/snes/rc_dispatch.c` | **NEW** — per-bank hash dispatch (Knuth mult, LF~0.5) | `6718ad7` |
| `src/snes/rc_dispatch.h` | **NEW** — dispatch API (`g_rc_active`, `rc_dispatch_*`) | `6718ad7` |

### Top-level

| File | Change | Commit |
|------|--------|--------|
| `Core/Src/porting/snes/main_snes.c` | `spin_whitelist_set()` call + `rc_smw_activate()` | `0533ac9f`, `1c6cd6e3` |
| `Core/Src/porting/snes/rc_smw_sites.c` | **NEW** — XIP compilation unit (sites + cpu_copy + header) | `1c6cd6e3` |
| `STM32H7B0VBTx_SDCARD.ld` | `.xip_rc_smw` section at `RCSMW_CODE` sentinel (0xD1D00000) | `1c6cd6e3` |
| `Makefile` | `RCSMW ?= 0` gate + `rc_dispatch.c` in source list | `1c6cd6e3` |
| `Makefile.common` | rc_smw compile rule + objcopy extraction + sdpush | `1c6cd6e3` |
| `sm_redefines` / `snes_redefines` | rc dispatch symbols for cross-overlay isolation | `1c6cd6e3` |
| `generated/rc_smw/rc_sites.inc` | **NEW** (2.4MB) — 8,371 site functions | `1c6cd6e3` |
| `generated/rc_smw/cpu_copy.c` | **NEW** (64KB) — interpreter copy for XIP blob | `1c6cd6e3` |

---

## Next Steps

1. **rc-SMW device measurement** — flash RCSMW=1 build + rc_smw.xip → measure
   SMW fps on hardware. The rig proves −42.3% algorithmic reduction; device
   cache/bus overhead from XIP execution via QSPI is the unknown.
2. **rc expansion** — if SMW device result is positive, the rc pipeline
   (`tools/sfc_recomp/build.sh <rom>`) can target other high-value ROMs. Each
   ROM needs its own `rc_<rom>.xip` + CRC detection + dispatch table.
3. **Spin-skip survey** — run `run_snes_spin.sh` on the 2,097 bootable ROMs to
   build a complete skip% table → expand the whitelist beyond SMW.
4. **DMA2D offload** (future ticket) — STM32H7B0 has a DMA2D (Chroma Art
   Accelerator) that can do L8→RGB565 via CLUT natively. Could offload the
   815K insn/frame output loop to hardware. Architectural change — deferred.
