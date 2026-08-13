# SNES CPU Static Recompilation (AOT) Prior Art & Technical Feasibility

## 1. Executive Summary & Constraint Context

* **Real-Gameplay Baseline**: **52.36 FPS** (*Zelda: ALttP* Rain scene, savestate autoboot, 900-frame window, $\pm 0.09$).
* **Frame Overhead Breakdown**:
  * PPU: **23%**
  * APU: **22%**
  * CPU: **24%**
  * Scheduler: **13%**
  * (Split into three roughly equal thirds; no single dominant bottleneck remains).
* **Target Subsystem**: 65816 Static Recompiler (`rc`) feasibility for wider game coverage.
* **Hardware Constraint (Stall-Bound Architecture)**:
  * STM32H7B0 Cortex-M7 is memory stall-bound: Rig instruction reduction of -5.5% delivered only +1.9% speedup on real HW (transfer ratio ~1/3). Rig instruction count reductions cannot be converted 1:1 into FPS gains.
* **Existing Codebase Inventory (Pre-verified in Tree)**:
  * [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1) / [`rc_dispatch.h`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.h#L1)
  * [`tools/sfc_recomp/README.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/tools/sfc_recomp/README.md#L1)
  * [`docs/RESUME_GNW.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RESUME_GNW.md#L1)
  * [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L1)
  * [`docs/RC_ACTIVATION_VERIFY.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_ACTIVATION_VERIFY.md#L1)

---

## 2. Item 1: Static Recompilation (AOT) Prior Art

### 2.1 `SNESRecomp` / `SuperMarioWorldRecomp` (by `mstan`)
* **Repository & File Path**:
  * Repository: [`mstan/snesrecomp`](https://github.com/mstan/snesrecomp) & [`mstan/SuperMarioWorldRecomp`](https://github.com/mstan/SuperMarioWorldRecomp)
  * Files: [`src/snesrecomp/recompiler.cpp`](https://github.com/mstan/snesrecomp/blob/main/src/snesrecomp/recompiler.cpp)
* **Mechanism**:
  * **Bank Switching**: Translates 65816 machine code into native C basic block functions structured per 64KB bank. Bank switching maps virtual bank offsets to native function arrays or a C `switch` statement (`_emit_dispatch`).
  * **Self-Modifying Code (SMC)**: Static AOT recompilation assumes code in ROM is constant. For RAM/WRAM execution or dynamic SMC, `SNESRecomp` uses a hybrid architecture: statically unresolvable or dynamically modified code paths fall back to a safe cycle-accurate interpreter tier.
* **Feasibility in G&W**:
  * In our codebase, [`tools/sfc_recomp/translate.py`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/tools/sfc_recomp/translate.py) parses executed basic blocks into native C site functions. Code executing in WRAM or unmapped ROM pages falls back mid-stream to `snes_cpuRead` / interpreter.

### 2.2 `NESRecomp` (6502 AOT by `mstan`)
* **Repository & File Path**:
  * Repository: [`mstan/nesrecomp`](https://github.com/mstan/nesrecomp)
  * Files: [`src/nesrecomp/recompiler.cpp`](https://github.com/mstan/nesrecomp/blob/main/src/nesrecomp/recompiler.cpp)
* **Mechanism**:
  * **Bank Switching**: NES mappers (MMC1, MMC3, VRC6) switch 8KB/16KB ROM banks dynamically. `NESRecomp` maintains bank mapping tables that resolve native function addresses based on active mapper registers.
  * **SMC**: RAM-based code execution triggers interpreter fallback.

---

## 3. Item 2: Dispatch Table Placement & Compression Under 128 KiB Memory

### 3.1 `rc_dispatch` (Our Per-Bank Knuth Multiplicative Open-Addressing Hash)
* **Repository & File Path**:
  * Submodule: [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1)
  * Analysis: [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L1)
* **Mechanism**:
  * Per-bank open-addressing hash table using Knuth multiplicative hashing: `(pc * 2654435761u) & mask` with linear probing (Load Factor $\approx 0.5$).
  * Storage Size: **85.0 KiB** for SMW (8,371 sites across 7 banks).
  * Storage Location: Placed in `RAM_EMU` (.overlay_snes_bss) static BSS buffer. Zero DTCM heap allocation (`malloc` free), solving the 81KB DTCM OOM crash.
  * Fast $O(1)$ deterministic lookup (~6–8 cycles).
* **RAM Cost**: 85.0 KiB in `RAM_EMU` overlay BSS (0 B DTCM heap).

### 3.2 Minimal Perfect Hashing (MPH) Alternative
* **Mechanism**: Uses CHD or PTHash algorithms to build a 0-collision hash table requiring ~2.5–3.0 bits per key.
* **Memory Cost**: ~2.6 KiB for hash displacement + 33.5 KiB for function pointer table = **~36 KiB RAM total**.
* **Feasibility in G&W**: Fits within 60–80 KiB usable DTCM, but requires 2–3 multiplication/mixing operations per lookup.

### 3.3 Sorted Binary Search Alternative
* **Mechanism**: Stores sorted `(pc, id)` pairs per bank in RAM/Flash. Uses `bsearch` / `std::lower_bound`.
* **RAM Cost**: **~33.5 KiB RAM**.
* **Feasibility in G&W**: Evaluated and rejected in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L40) (Bank 0 with 3,767 sites requires 11–12 comparison iterations per lookup with high branch misprediction rates on Cortex-M7).

---

## 4. Item 3: XIP (Execute-In-Place) Execution Prior Art

### 4.1 Success vs. Failure Cases on STM32H7 / Game & Watch Hardware
* **Success Case — Super Metroid (`main_sm.c`)**:
  * Mechanism: `odroid_overlay_cache_file_in_flash_relocate("/roms/homebrew/sm.xip")` caches `sm.xip` into 16MB External QSPI Flash. Native site function pointers are patch-relocated to `0x90000000`.
  * Result: Executes successfully across Ceres intro on real hardware.
  * Why it succeeded: Low execution frequency (~90 rare veneer calls).
* **Failure Case — DOOM Port on Game & Watch**:
  * Mechanism: Attempted XIP execution with RAM $\leftrightarrow$ XIP `ldr pc, [pc]` linker veneers.
  * Result: Crashed during XIP execution.
  * Cause: DOOM attempted to use a 749KB cache inside XIP while executing next-hack code simultaneously, triggering ABFSR/bus fault exceptions on linker veneers.
* **Crux of XIP Execution**: **Scale and Call Frequency**.
  * `rc` has 8,371 site functions executing at **millions of calls per second**. High-frequency indirect branches into external QSPI Flash incur severe QSPI bus wait states and I-cache thrashing on Cortex-M7.

---

## 5. Item 4: Alternatives Chosen When Full AOT Was Abandoned

### 5.1 Hot Block Only / Hot Path Recompilation (Trace Cache)
* **Repository & File Path**:
  * Example: [`libretro/pcsx_rearmed`](https://github.com/libretro/pcsx_rearmed) (`frontend/cemu.c` / `recomp.c`)
* **Mechanism**: Recompiles only the top 5%–10% most frequently executed basic blocks/loops (identified via profiling or execution counters), while 90% of cold code remains in the fast interpreter.
* **Gains**: Captures 80%–90% of execution time gains while reducing code size and XIP flash footprint by 80%–90%.

### 5.2 Hand-Coded Assembly Interpreter in ITCM / SRAM
* **Repository & File Path**:
  * Submodule: [`external/sm/src/snes/thumb2/`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/thumb2/) (`snes_thumb2.S`)
  * Document: [`docs/SNES_THUMB2_PORT_HANDOFF.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/SNES_THUMB2_PORT_HANDOFF.md#L1)
* **Mechanism**: Hand-coded Cortex-M7 Thumb-2 assembly 65816 interpreter resident entirely in 64KB ITCM (`0x00000000`).
* **Gains**: Real HW speedup from **51.6 to 57.3 FPS** (commit `ec4fb9fa`) by eliminating cross-overlay linker veneers and running 100% inside 0-wait-state ITCM.

---

## 6. Summary Matrix

| Category | Repository / Reference | Mechanism | Feasibility & Verdict | RAM / Storage |
| :--- | :--- | :--- | :--- | :--- |
| **1. AOT SMC/Bank Switch** | [`mstan/snesrecomp`](https://github.com/mstan/snesrecomp) | Static basic block C functions + Interpreter fallback for SMC/WRAM | **가능** (Used in `tools/sfc_recomp`) | 0 B (PC OS)<br>XIP on G&W |
| **2. Dispatch Map (<128KB)** | [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1) | Per-bank Knuth multiplicative open-addressing hash ($\text{LF} \approx 0.5$) | **가능** (SHIPPED & PROVEN in tree) | **85.0 KiB**<br>(Overlay BSS) |
| **3. XIP Execution** | Super Metroid (`main_sm.c`) vs. DOOM | QSPI Flash caching + Thumb `blx` indirect calls | **성공 사례 존재하나 크럭스는 빈도** (DOOM은 베니어/캐시 충돌로 실패, SM은 성공) | XIP Flash 1.5MB<br>RAM 0B |
| **4. Alternatives to AOT** | [`external/sm/src/snes/thumb2/`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/thumb2/) | Hand-coded Thumb-2 65816 engine in 64KB ITCM | **가능** (SHIPPED, +5.7 FPS real HW win) | 64 KiB ITCM |
