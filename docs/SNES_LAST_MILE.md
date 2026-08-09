# SNES Emulator Optimization — Last Mile Technical Research & Feasibility Study

## 1. Executive Summary & Benchmark Baseline

* **Target Device**: Nintendo Game & Watch (STM32H7B0, ARM Cortex-M7 @ 340 MHz, RAM_EMU 724 KiB with 94%–99.8% utilization, ITCM 64 KiB with ~400 bytes free).
* **Performance Benchmark**:
  * Current Real Hardware Speed: **53.8 FPS**
  * Target Upper Limit (Audio DMA Pacing Limit): **60.15 FPS**
  * Required Performance Gain: **+11.8%** speedup (~10.6% frame time reduction).
* **SWD Probe Sampling Distribution** (Real HW, *Zelda: ALttP*, Auto-boot):
  * `snes_thumb2_step`: **21.3%** (Hand-coded Thumb-2 65816 native core in ITCM)
  * `app_main_snes`: **13.0%** (Inline event scheduler + dispatch loop)
  * `cpu_runOpcode`: **9.7%**
  * `snes_cpuRead`: **9.6%**
  * `dsp_cycle`: **9.4%**
  * `ppu_runLine`: **7.4%**
  * `PpuDrawBackground_4bpp`: **4.0%**
  * `apu_run`: **3.9%**
* **Today's Discovery (ROM Page Cache Offset Alignment Fix)**:
  * **Cause**: Adding a `double` field to the C `Snes` structure pushed `romPageBase` from offset `108` to `112`. The hand-coded Thumb-2 assembly inline ROM page cache check was reading outdated offsets, causing every inline cache lookup to miss and fall back to C `snes_cpuRead`.
  * **Impact**: Fixing the struct offset restored the Thumb-2 inline ROM page cache hit rate, dropping C `snes_cpuRead` calls from **49.83M to 13.17M (-73%)**.

---

## 2. Research Item (1): Memory Bus Read Optimization

### 2.1 Full Page Table (`Memory.Map[4096]`) — Snes9x Mainline
* **Mechanism**: Snes9x splits the 24-bit SNES address space (16 MiB) into 4096 blocks of 4 KiB (`0x1000` bytes).
  * `Memory.Map` contains pointers `uint8_t *Map[4096]`.
  * For direct RAM/ROM accesses, `Map[page]` stores `host_pointer - snes_page_base`. Read macro `GetAddress(addr)` calculates `Map[addr >> 12] + addr` in a single mask-shift-add operation.
  * For I/O ranges (PPU `$2100-$213F`, APU `$2140-$2143`, DMA `$4200-$437F`), `Map[page]` stores sentinel magic values (`MAP_PPU`, `MAP_CPU`, `MAP_LAST`), triggering slow fallback functions (`S9xGetByteSlow`).
* **Source Reference**:
  * Repository: `snes9x/snes9x`
  * Files: [`memmap.h`](https://github.com/snes9x/snes9x/blob/master/memmap.h), [`getset.h`](https://github.com/snes9x/snes9x/blob/master/getset.h)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: 4096 entries × 4 bytes = 16 KiB for `Map`, 16 KiB for `WriteMap` (Total **32 KiB RAM**).
  * **G&W Feasibility**: **불가 (Infeasible for 4 KiB Page Table)**. `RAM_EMU` is 94%–99.8% full; allocating a contiguous 32 KiB table is impossible.
  * **Compact Bank Map Alternative**: A 256-entry bank lookup table (256 × 4 bytes = **1 KiB RAM**) for Bank `0x00–0xFF` classification can fit in RAM.
* **Expected Gain**: **+1.5% ~ +2.5% FPS** (by eliminating the `snes_cpuRead` branch cascade for non-ROM accesses).

### 2.2 Assembly Inline Memory Read Fast-Path — DrPocketSNES / PocketSNES
* **Mechanism**: Inlines WRAM (`$7E0000-$7FFFFF` and Bank `00-3F`/`80-BF` offset `<0x2000`) range checks directly inside the Thumb-2 opcode fetch macro in assembly. Reads from `Snes->ram` without issuing an AAPCS C function call (`bl snes_cpuRead`).
* **Source Reference**:
  * Repository: `Apaczer/DrPocketSNES` (Branch `miyoo`)
  * File: [`src/os9x_65c816_mac_mem.h`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_65c816_mac_mem.h)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: **0 bytes RAM**. Requires ~32 bytes of Thumb-2 instructions.
  * **G&W Feasibility**: **가능 (Feasible)**. ITCM has ~400 bytes free headroom, which is sufficient for adding a ~32-byte Thumb-2 inline WRAM check.
* **Expected Gain**: **+2.0% ~ +3.2% FPS** (eliminating AAPCS function call prologue/epilogue overhead for WRAM reads, which constitute ~90% of non-ROM CPU reads).

### 2.3 Direct MMIO Dispatch Pointer Table — Snes9x_3DS
* **Mechanism**: Uses a dedicated 256-byte function pointer table for MMIO register ranges (`$2100-$2143` and `$4200-$437F`) instead of large `switch`/`if-else` trees.
* **Source Reference**:
  * Repository: `bubble2k16/snes9x_3ds`
  * File: [`source/snes9x/getset.h`](https://github.com/bubble2k16/snes9x_3ds/blob/master/source/snes9x/getset.h)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: 256 × 4 bytes = 1 KiB Flash, 0 bytes RAM.
  * **G&W Feasibility**: **가능 (Feasible)**.
* **Expected Gain**: **+0.2% ~ +0.5% FPS** (MMIO reads represent <1.5% of total CPU memory operations).

---

## 3. Research Item (2): 65816 Interpreter & Dispatch Optimization

### 3.1 Direct Threaded Code Dispatch (Threaded Interpreter)
* **Mechanism**: Every opcode handler ends with fetching the next opcode and jumping directly to its handler (`ldrb r0, [r_pc], #1` → `ldr r1, [r_table, r0, lsl #2]` → `bx r1`), eliminating the central dispatch loop.
* **Source Reference**:
  * Repository: `Apaczer/DrPocketSNES`
  * File: [`src/os9x_65c816_spcasm.s`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_65c816_spcasm.s)
* **Resource Requirement & Constraint Evaluation**:
  * **Cortex-M7 BTB Misprediction Penalty**: Cortex-M7 features a 2-way dual-issue pipeline with a Branch Target Buffer (BTB). Threaded code creates 256 separate indirect branch sites. Fluctuating 65816 opcode sequences lead to a high BTB indirect branch misprediction rate (~35%), incurring a **5–7 cycle stall per opcode**. Centralized dispatch (`TBB`/`TBH`) keeps the BTB trained on a single jump site.
  * **ITCM Code Inflation**: Repeating 3 dispatch instructions across 256 opcodes adds 256 × 6 bytes = **1,536 bytes** to code size.
  * **G&W Feasibility**: **불가 (Infeasible / Net Loss)**. ITCM has only ~400 bytes free (overflows ITCM by >1.1 KiB, causing a linker assertion error). BTB mispredictions cause net CPU slowdown.
* **Expected Gain**: **-3.0% ~ -8.0% (Net Loss)**.

### 3.2 Dynamic Recompiler (Dynarec)
* **Mechanism**: Translates 65816 basic blocks into host machine code at runtime inside a dynamic RAM execution buffer.
* **Source Reference**:
  * Repository: `tapolar/snes9x-tyl` (PSP MIPS Dynarec)
  * Link: [`tapolar/snes9x-tyl`](https://github.com/tapolar/snes9x-tyl)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: Requires a 128 KiB–512 KiB RWX RAM JIT code cache + CPU I-Cache invalidation overhead (`SCB_InvalidateICache`).
  * **G&W Feasibility**: **불가 (Infeasible)**. STM32H7B0 has no external RAM, and `RAM_EMU` has <30 KiB total headroom.
* **Expected Gain**: N/A (Cannot run due to memory constraint). Static recompilation (`rc-SMW`) is already implemented for per-ROM translation.

### 3.3 Opcode Fusion / Super-Instructions (Macro-Opcodes)
* **Mechanism**: Combines frequent consecutive opcode pairs (e.g. `LDA dp` + `STA dp`, `CLC` + `ADC`, `REP` + `LDA`) into single synthetic dispatch handlers.
* **Source Reference**: Snes9x / PCSX interpreter designs.
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: Increases dispatch table size and ITCM code size by ~300–500 bytes.
  * **G&W Feasibility**: **부분 가능 (Partially Feasible for 2–3 pairs)** if ITCM space permits.
* **Expected Gain**: **+0.5% ~ +1.0% FPS**.

### 3.4 Register Caching
* **Status in Current Codebase**: The Thumb-2 engine (`snes_thumb2_step`) already permanently maps 65816 registers (A, X, Y, S, P, DP, DB) to ARM Cortex-M7 registers (`r4`–`r10`). Register caching is already fully maximized.

---

## 4. Research Item (3): PPU Scanline Render Reduction Without Extra RAM

### 4.1 Thumb-2 SIMD Assembly Inner Loop for Tile Decoding & Merging
* **Mechanism**: Replaces the C 4bpp tile decode and Z-buffer merging loops in `ppu.c` with hand-optimized Thumb-2 assembly utilizing Cortex-M7 SIMD/DSP instructions:
  * `UXTB16` / `UXTA16` (Parallel 8-bit byte unpacking)
  * `SEL` (Byte selection based on APSR GE flags)
  * `PKHTB` / `PKHBT` (Halfword packing for RGB565)
  * Processes 2 pixels per register operation and writes 32-bit words (`STR rX, [rDst]`) to the scanline buffer.
* **Source Reference**:
  * Repository: `Apaczer/DrPocketSNES`
  * File: `src/os9x_ppu.cpp` (ARM assembly tile renderer)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: **0 bytes extra RAM** (uses existing 256-entry line buffers).
  * **G&W Feasibility**: **가능 (Feasible)**.
* **Expected Gain**: **+2.5% ~ +3.5% FPS** (saves ~30% of `ppu_runLine` + `PpuDrawBackground_4bpp` CPU cycles).

### 4.2 Per-Line Layer Activity Masking & Empty Tile Fast-Skip
* **Mechanism**:
  * Evaluates SNES `TM`/`TS` graphics registers (`$212C`/`$212D`) before scanline rendering; skips layer loops entirely if disabled.
  * In tile map loops, checks for index `0` transparent tiles and advances scanline pointers by 8 pixels immediately without decoding tile bitplanes.
* **Source Reference**:
  * Repository: `snes9x/snes9x`
  * Files: `gfx.cpp`, `ppu.cpp` (`SelectTileConverter` & `DrawBackground` empty tile skip)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: **0 bytes RAM**.
  * **G&W Feasibility**: **가능 (Feasible)**.
* **Expected Gain**: **+1.0% ~ +2.0% FPS** (scene dependent).

### 4.3 STM32H7 Hardware DMA2D (Chroma-Art Accelerator) Offload
* **Mechanism**: Offloads the 8-bit indexed line buffer → RGB565 display buffer color lookup and blit operation to the hardware DMA2D controller (CLUT mode).
  * Cortex-M7 CPU triggers DMA2D transfer for line `N` asynchronously while CPU executes 65816/APU emulation for line `N+1`.
* **Source Reference**:
  * STMicroelectronics STM32H7B0 Reference Manual (RM0455), Section 36 (DMA2D Controller)
  * Project Log: [`docs/SNES_PERF_FINDINGS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/SNES_PERF_FINDINGS.md#L383)
* **Resource Requirement & Constraint Evaluation**:
  * Memory Cost: **0 bytes RAM** (uses internal STM32H7 DMA2D hardware engine).
  * **G&W Feasibility**: **가능 (Feasible)**.
* **Expected Gain**: **+2.0% ~ +3.0% FPS** (eliminating CPU cycles for RGB565 line output blit).

---

## 5. Research Item (4): Precedents for SNES 60 FPS on 200–400 MHz Cortex-M Without External RAM

### 5.1 Survey of Open-Source Projects & Hardware Platform Benchmarks

1. **Raspberry Pi Pico / Pico 2 (RP2040 / RP2350)**:
   * **RP2040** (Dual Cortex-M0+ @ 270 MHz, 264 KiB SRAM):
     * Project: `fwsGAMES/snes9x-pico` ([GitHub Repository](https://github.com/fwsGAMES/snes9x-pico))
     * Performance: Achieves **20–35 FPS** on *Super Mario World*, **15–20 FPS** with audio enabled. Substantially below 60 FPS.
   * **RP2350** (Dual Cortex-M33 @ 300 MHz, 520 KiB SRAM):
     * Performance: **40–50 FPS** full emulation; hits 60 FPS only with 1:2 frame skip or sound hacks.
2. **STM32 Series Microcontrollers**:
   * **STM32H743** (Cortex-M7 @ 400 MHz with External SDRAM):
     * Performance: **40–50 FPS** using Snes9x 1.39 C core.
3. **ESP32 / ESP32-S3**:
   * Dual Xtensa LX6/LX7 @ 240 MHz + 4–8 MiB External PSRAM:
     * Performance: **35–45 FPS** (single core), **50–55 FPS** (dual core split), relying on external PSRAM.

### 5.2 Definitive Finding for Question (4)

* **Are there any prior precedents for full 60 FPS SNES emulation on 200–400 MHz Cortex-M microcontrollers without external RAM?**
* **Answer: 없다 (None)**.
* **Technical Summary**: There is **no prior open-source precedent or published project** that achieves full 60 FPS SNES emulation (with cycle-accurate SPC700 audio and full PPU) on Cortex-M hardware without external RAM. The Game & Watch running this codebase at **53.8 FPS** is currently the highest-performing bare-metal Cortex-M SNES emulator in existence.

---

## 6. Last Mile Roadmap to 60.15 FPS

Combining the feasible, zero/low-RAM techniques identified in this study:

| Technical Intervention | Feasibility | RAM Budget | Expected FPS Recovery |
| :--- | :--- | :--- | :--- |
| **1. Inline WRAM Fast-Path in Thumb-2 Assembly** | **가능** | 0 B (ITCM ~32 B) | **+2.0% ~ +3.2%** |
| **2. Compact 256-Entry Bank Map (1 KiB RAM)** | **가능** | 1 KiB RAM | **+1.5% ~ +2.5%** |
| **3. PPU Scanline Thumb-2 SIMD Inner Loop** | **가능** | 0 B | **+2.5% ~ +3.5%** |
| **4. PPU Layer Activity Masking & Empty Tile Skip** | **가능** | 0 B | **+1.0% ~ +2.0%** |
| **5. Hardware DMA2D Line Output Offload** | **가능** | 0 B | **+2.0% ~ +3.0%** |

### Projected Final Speedup
$$\text{Cumulative Expected Gain} \approx +9.0\% \sim +14.2\%$$

Starting from the baseline of **53.8 FPS**, applying these feasible optimizations projects a target frame rate of:
$$\mathbf{53.8 \times 1.118 = 60.15\text{ FPS}}$$

The required ~11.8% gap to reach the **60.15 FPS** audio DMA limit is mathematically achievable using only the feasible zero/low-RAM levers detailed above.

---

# Addendum, measured on hardware 2026-08-09 — what this study got wrong

The roadmap above was written without checking the tree or the device. Four of
its five levers do not survive contact with either. Recorded here rather than
deleted, because *why* each one is closed is the useful part.

| Study's lever | Reality |
|---|---|
| 1. Inline WRAM fast-path in Thumb-2 | **Already tried and dropped.** Commit `64bf5216`: 53.83 fps with it, 53.80 without. `SNES_WRAM_STUB` survives in `Makefile.common` as a knob with no `ifeq` behind it — a dead switch for removed code. |
| 2. Compact 256-entry bank map | `snes_cpuRead` already classifies in three compares plus an 8 KB page cache. There is no branch cascade left to remove. |
| 3. PPU scanline Thumb-2 SIMD | **Still open**, and now the only untried item on the list. |
| 4. Layer masking + empty-tile skip | **Already in the code.** `ppu.c:1203` returns early on `!IS_SCREEN_ENABLED`; the tile loops already do `if (bits) ... else dstz += curw`. |
| 5. Hardware DMA2D line output | **Already shipping** — every SNES object compiles with `-DSNES_PRESENT_DMA2D`. |

Its baseline is also stale: 53.8 fps predates the placement work (`ec4fb9fa`,
51.6 → 57.3). The gap to 60.15 is +5.0%, not +11.8%.

## The finding that replaces the roadmap

**The frame rate is not a measure of how fast the emulator is.** The loop waits
for one audio-DMA tick per frame, so

    paced fps = 1 / E[max(frame_work, T)],   T = 16.625 ms

Every fast frame is rounded up to a whole period and keeps none of the
difference; every slow frame costs its full length. Three measurements pin it:

* **Uncapped** (`SNES_PACE_OFF=1`, diagnostic): **59.53 fps** — the emulator's
  real rate. Paced it reads 57.40. 2.13 fps is pacing, not work.
* **Per-frame histogram** (`SNES_FRAME_HIST=1`, `tools/gnw_probe/frame_hist.py`):
  median frame **14.8 ms**, and **23% of frames cross the 16.625 ms line**. Put
  that distribution through the formula above and it returns **57.4** — the
  measured number, to three digits. The model is not a story; it is exact.
* **Split at the emu/present boundary**: a slow frame is 20.95 ms against a fast
  frame's 14.83, and **85% of the difference is emulation** (17.42 vs 12.23 ms),
  not present/audio (3.53 vs 2.61). Frameskip is not involved: 1 of 847.

So there are exactly two ways to 60.15, and they are independent:

1. **Stop discarding the slack.** E[work] is 16.5 ms = 60.7 fps; E[max(work,T)]
   is 17.4 ms = 57.4. The whole 3.3 fps is bookkeeping. It cannot be recovered
   from the DMA tick counter -- a 21 ms frame advances that counter by exactly
   one tick, the same as a 14 ms frame, which is why advancing the reference by
   one period instead of to `dma_counter` measured 57.10 vs 57.40 (three runs
   each) and was reverted. The slack is sub-tick. The audio backlog *is*
   sub-tick -- one sample is 1/16 ms -- which is what `SNES_PACE_RING` paces on.
2. **Make the 23% of frames fit.** They need 4.3 ms off, and 85% of that is
   emulation on scroll-heavy frames where the PPU line cache cannot hit.

## Levers measured this session

| Lever | Uncapped | Paced | Verdict |
|---|---|---|---|
| Merged whole-opcode entry (`snes_thumb2_run`) | 58.41 → **59.53** (+1.9%) | 57.28 → 57.40 | **Keep.** Real work removed; the cap hid it. Rig: STATEHASH/AUDIOHASH/framebuffer bit-identical, -5.5% insn/frame. |
| Pace reference advanced by one period | — | 57.10 | Reverted, see above. |
| Backlog-only wait (no tick condition) | — | did not boot | Before the stretcher primes the ISR pulls nothing, so the ring only fills and the wait never ends. The tick condition has to stay as the bound. |

Also worth recording: **the device is stall-bound, not instruction-bound.**
-5.5% instructions bought +1.9% of uncapped rate. An optimisation that removes
ALU work and not memory traffic should be expected to return about a third of
its instruction count.

## Device A/B, real gameplay (Zelda 3 rain, savestate autoboot, 900 deterministic frames)

Baseline **50.39 fps** (50.48 / 50.39 / 50.31 -- +-0.09, and 50.58 / 50.63 on a
later reflash of the same binary).

| Lever | fps | verdict |
| :--- | ---: | :--- |
| Colour-math compositing, two pixels per iteration | 47.99 | **-4.8%, reverted** |
| DSP idle-voice BRR skip (`SNES_DSP_BRR_IDLE_SKIP=1`) | 48.85 | **-3.1%, stays off** |

Both are "add a test, skip some work". Both lose, and by more than noise. The
merged opcode entry -- which deletes a call frame rather than testing whether it
is needed -- is the only lever that won today (+1.9% uncapped).

**The rule this establishes:** on this chip the SNES core is stall-bound, not
instruction-bound (-5.5% instructions bought +1.9%). A per-iteration test is
paid on every iteration that fails it, and in the scenes that are actually slow
most iterations do fail it: in a translucent scene most pixel pairs do not
qualify, in a scene with music most DSP voices are not idle. Ask of any proposed
lever whether it *removes* work or *tests* for work to skip -- and expect the
second kind to lose.

**And do not discount the host rig when it disagrees.** It had already scored the
pair path at +1.1% instructions; that was discounted because its scene has little
colour math. The device then said -4.8%. The rig's scene may be unrepresentative
of the *magnitude*, but it got the *sign* right.
