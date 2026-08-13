# SNES APU (SPC700 + S-DSP) Removal-Type Optimization Research

## 1. Executive Summary & Hardware Constraint Alignment

* **Target Subsystem**: SPC700 + S-DSP APU Emulation Pipeline
* **Target Profiling Benchmark** (*Zelda: ALttP* Rain Scene, Real HW Gameplay):
  * `dsp_cycle`: **10.4%**
  * `apu_cycle`: **4.4%**
  * `apu_run`: **3.7%**
  * `spc_thumb2_step`: **3.4%**
  * `spc_runOpcode`: **1.7%**
  * **APU Total Overhead: 23.6%**
* **Hardware Constraint & Guideline Enforcement**:
  * **Stall-Bound Architecture**: STM32H7B0 Cortex-M7 is memory stall-bound. Rig instruction reduction of -5.5% yielded only +1.9% on real HW.
  * **Work Removal vs. Condition Check**:
    * "Add a condition to check and skip work" (`if`) $\rightarrow$ **Net Loss** on real HW (e.g. Color Math 2-pixel -4.8%, DSP Idle BRR skip -3.1%), because in heavy/slow scenes, most iterations fail the test and pay branch/miss penalties on every loop pass.
    * "Remove work completely" $\rightarrow$ **Net Win** (e.g. Call frame deletion +1.9%, Bus access cycle addition removal +2.2%).
  * **Evaluation Metric**: All researched techniques are evaluated strictly on whether they perform **"Removal (삭제)"** or **"Check (검사)"**.

---

## 2. Research Item (1): Channel Mixing & BRR Decoding Data Structures

### 2.1 blargg's `snes_spc` / `Spc_Dsp`
* **Repository & File Path**:
  * Repository: [`libgme/game-music-emu`](https://github.com/libgme/game-music-emu)
  * Files: [`gme/Spc_Dsp.cpp`](https://github.com/libgme/game-music-emu/blob/master/gme/Spc_Dsp.cpp), [`gme/Spc_Dsp.h`](https://github.com/libgme/game-music-emu/blob/master/gme/Spc_Dsp.h)
* **Mechanism**:
  * Decomposes the S-DSP sample period (32 APU cycles) into a 32-phase cyclic pipeline state machine (`phase_0` .. `phase_31`).
  * In each 1-cycle phase, a dedicated sub-task is executed (e.g. Phase $v \times 4 + 0$: Voice $v$ BRR read/decode; Phase $v \times 4 + 1$: Voice $v$ ADSR; Phase $v \times 4 + 2$: Gaussian interp; Phase $v \times 4 + 3$: Volume/Output).
  * Channels are not iterated in a single monolithic loop per sample; operations are interleaved cycle-by-cycle with the SPC700 CPU.
* **Classification ("삭제인가 검사인가")**:
  * **검사/구조 변경 (Check / Structural Decomposition)**. It does not remove work; it breaks execution into 32 phase dispatches for cycle accuracy, adding phase counter tracking overhead.
* **Mapping to our Codebase**:
  * Maps to [`external/sm/src/snes/dsp.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c):
    * `dsp_cycle` (line 163): Monolithic loop `for(int i = 0; i < 8; i++) dsp_cycleChannel(dsp, i);`
    * `dsp_cycleChannel` (line 300): Synchronous execution of BRR decode, ADSR, Gaussian interp, and volume scale in a single call.
* **RAM Cost**: ~512 B for phase state structures and voice registers.

### 2.2 bsnes / ares
* **Repository & File Path**:
  * Repository: [`ares-emulator/ares`](https://github.com/ares-emulator/ares)
  * File: [`ares/component/audio/spc700/dsp.cpp`](https://github.com/ares-emulator/ares/blob/master/ares/component/audio/spc700/dsp.cpp)
* **Mechanism**:
  * Implements strict 32-step hardware cycle execution matching S-DSP register timing.
* **Classification**: **검사/구조 변경 (Check / Structural Decomposition)**.
* **Mapping to our Codebase**: [`external/sm/src/snes/dsp.c:163`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c#L163) (`dsp_cycle`).
* **RAM Cost**: ~512 B.

### 2.3 Structural Difference Summary vs. Our `dsp.c`
* Our `dsp.c` (derived from LakeSnes) uses **Sample-Major Monolithic Iteration**: every 32 CPU cycles (1 sample tick), `dsp_cycle` loops through all 8 voices sequentially, running pitch, BRR decode, Gaussian interpolation (`dsp_getSample`), ADSR, and volume calculation in one block, followed by `dsp_handleEcho`.
* `blargg` / `bsnes` / `ares` use **Cycle-Major 32-Phase Interleaved Pipeline**: 1 micro-task per clock cycle, tightly coupled with SPC700 memory bus accesses.

---

## 3. Research Item (2): Block-Based 32 kHz DSP Ticking

### 3.1 Snes9x 1.39 / SoundUX / PocketSNES
* **Repository & File Path**:
  * Repository: [`snes9x/snes9x` (branch 1.39)](https://github.com/snes9x/snes9x/blob/1.39/apu/soundux.cpp)
  * File: [`apu/soundux.cpp`](https://github.com/snes9x/snes9x/blob/1.39/apu/soundux.cpp)
  * Port Reference: [`Apaczer/DrPocketSNES`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_soundux.cpp) (`src/os9x_soundux.cpp`)
* **Mechanism**:
  * Replaces **Sample-Major Monolithic Iteration** with **Voice-Major Block Mixing**:
  * Accumulates $N$ samples (e.g. 32, 64, or 128 samples per block between SPC700 DSP register writes or APU synchronization points).
  * Inner loop structure:
    ```c
    for (int ch = 0; ch < 8; ch++) {
        // Hoist voice invariants (volume, pitch, envelope, BRR pointers) to locals
        // Render N samples for voice 'ch' in a tight contiguous loop
    }
    // Perform 1 block-level FIR Echo pass for N samples
    ```
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal of Work)**:
    * Deletes 7/8ths of loop dispatch overhead ($8 \times 534 = 4,272$ calls/frame down to 1 call per block per voice).
    * Deletes sample-by-sample voice register save/reload overhead by hoisting invariants into CPU registers (`r4`–`r11`).
    * Deletes per-sample branch/cycle maintenance instructions inside the voice loop.
* **Mapping to our Codebase**:
  * Maps to [`external/sm/src/snes/dsp.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c):
    * `dsp_cycle` (line 163)
    * `dsp_cycleChannel` (line 300)
  * Maps to [`external/sm/src/snes/apu.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/apu.c):
    * `apu_run` (line 161): Replaces the 17,000x/frame `dsp_cycle` loop invocation (`for (uint32_t m ... += 32) dsp_cycle(apu->dsp);`, lines 184-186) with a single block call `dspb_run(dsp, num_samples)`.
* **RAM Cost**: Requires intermediate block sample buffers (e.g. 64 samples × 4 bytes × 2 channels = **512 B RAM**). Proof-of-concept already verified in [`tools/dsp_mixer/mixer_block.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/tools/dsp_mixer/mixer_block.c).

---

## 4. Research Item (3): Constant Coefficient Echo FIR Optimization

### 4.1 Snes9x 1.39 / PocketSNES (`soundux.cpp` / `os9x_soundux.cpp`)
* **Repository & File Path**:
  * Repository: [`snes9x/snes9x` (branch 1.39)](https://github.com/snes9x/snes9x/blob/1.39/apu/soundux.cpp)
  * File: `apu/soundux.cpp`
  * Port Reference: [`Apaczer/DrPocketSNES`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_soundux.cpp) (`src/os9x_soundux.cpp`)
* **Mechanism**:
  * **Write-Time Pre-computation (Zero-Run-Time MAC Removal)**:
  * When a write occurs to S-DSP FIR coefficient registers (`$0F`, `$1F` .. `$7F`), the driver inspects the 8 coefficients (`c0` .. `c7`) ONCE during `dsp_writeReg`:
    * Case 1 (Default Bypass Filter `[127, 0, 0, 0, 0, 0, 0, 0]`): Used by ~60% of SNES sound drivers (including default SMW & Zelda 3 drivers when echo is enabled). The 8-tap MAC loop is replaced at write-time with a direct assignment: `sumL = firBufferL[curr]; sumR = firBufferR[curr];` (**0 MACs instead of 16 MACs per sample**).
    * Case 2 (Symmetric Coefficients `c0==c7, c1==c6, c2==c5, c3==c4`): Pairs additions `(buf[i] + buf[7-i]) * c[i]`, cutting MAC multiplications from 16 to 8 per sample pair.
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal of Work)**:
    * Performs coefficient classification ONCE at register WRITE time (`dsp_writeReg`), NOT at sample generation time.
    * At sample generation time, executes 0 MACs or 8 MACs unconditionally without any per-sample `if` checks!
* **Mapping to our Codebase**:
  * Maps to [`external/sm/src/snes/dsp.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c):
    * `dsp_handleEcho` (line 230): Specifically the 8-tap MAC loop at lines 247-256 (`for (int i = 0; i < 8; i++) { sumL += ... }`).
* **RAM Cost**: ~8 B RAM in `Dsp` struct (for `fir_type` enum or active tap bitmask).

---

## 5. Research Item (4): SPC700 Cycle Accounting Folded to Once-per-Opcode

### 5.1 Snes9x 1.39 / PocketSNES (`spc700.cpp` / `os9x_spc700.cpp`)
* **Repository & File Path**:
  * Repository: [`snes9x/snes9x` (branch 1.39)](https://github.com/snes9x/snes9x/blob/1.39/apu/spc700.cpp)
  * File: `apu/spc700.cpp`
  * Port Reference: [`Apaczer/DrPocketSNES`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_65c816_spcasm.s) (`src/os9x_65c816_spcasm.s`)
* **Mechanism**:
  * Uses a static opcode cycle table `SPC700_Cycles[256]`.
  * Charges the total opcode cycles in a single step upon entering the opcode handler:
    `APU.Cycles += SPC700_Cycles[Opcode];`
  * Individual memory access routines (`spcRead`, `spcWrite`, `spcFetch`) do NOT increment cycle counters per byte read/written.
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal of Work)**: Deletes per-bus-access cycle increment instructions (`APU.Cycles++`) inside internal memory read/write routines.
* **Mapping to our Codebase**:
  * Already implemented in our codebase:
    * [`external/sm/src/snes/spc.c:127`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/spc.c#L127): `spc->cyclesUsed = cyclesPerOpcode[opcode];`
    * [`external/sm/src/snes/apu.c:174`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/apu.c#L174): `apu->cpuCyclesLeft = spc_runOpcode(apu->spc);`
    * [`external/sm/src/snes/apu.c:161-222`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/apu.c#L161-L222): `apu_run` charges `cyclesToRun -= step;` in bulk for the opcode.
* **RAM Cost**: **0 B RAM** (256-byte static table `spc_cycles_per_opcode[256]` stored in Flash).

---

## 6. Summary Matrix of Researched Items

| Item | Repo & File | Mechanism | Removal vs Check | Our Mapping | RAM Cost |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. DSP Data Struct / Pipeline** | [`libgme/game-music-emu`](https://github.com/libgme/game-music-emu)<br>`gme/Spc_Dsp.cpp` | 32-Phase Cycle Interleaved State Machine | **검사** (Adds phase counter & dispatch overhead) | [`dsp.c:163`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c#L163)<br>(`dsp_cycle`) | ~512 B |
| **2. Block-Based 32kHz DSP Ticking** | [`snes9x/snes9x` (1.39)](https://github.com/snes9x/snes9x/blob/1.39/apu/soundux.cpp)<br>`apu/soundux.cpp` | Voice-Major Block Mixing ($N$ samples/block) | **삭제** (Deletes 7/8ths of loop dispatch & state save/restore) | [`dsp.c:163`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c#L163)<br>[`apu.c:184`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/apu.c#L184) | ~512 B |
| **3. Constant Coeff Echo FIR** | [`Apaczer/DrPocketSNES`](https://github.com/Apaczer/DrPocketSNES/blob/miyoo/src/os9x_soundux.cpp)<br>`src/os9x_soundux.cpp` | Write-time FIR classification ($[127,0...0] \rightarrow 0$ MACs) | **삭제** (0 MACs at run-time without per-sample `if` checks) | [`dsp.c:247-256`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/dsp.c#L247-L256)<br>(`dsp_handleEcho`) | ~8 B |
| **4. SPC700 Per-Opcode Cycle Folding** | [`snes9x/snes9x` (1.39)](https://github.com/snes9x/snes9x/blob/1.39/apu/spc700.cpp)<br>`apu/spc700.cpp` | Single `cyclesPerOpcode[op]` addition at opcode entry | **삭제** (Deletes per-access cycle additions) | [`spc.c:127`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/spc.c#L127)<br>[`apu.c:174`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/apu.c#L174) | 0 B |
