# SNES CPU Static Recompilation Under Memory Constraints — Public Precedents & Feasibility Research

## 1. Executive Summary & Problem Definition

* **Problem Space**: Translating 65816 (SNES CPU) machine code to native host code via Static Recompilation (AOT) requires mapping dynamic PC jumps to native recompiled functions.
* **The Memory Constraint Challenge**:
  * Traditional PC lookups on modern PC platforms use flat lookup tables ($16\text{ MB} \sim 64\text{ MB}$ RAM), which is impossible on embedded platforms like STM32H7B0 (`RAM_EMU` 724 KiB, DTCM heap ~8 KiB available).
  * The core architectural challenge is designing a PC-to-Native-Site dispatch structure that achieves $O(1)$ fast lookup speed while keeping RAM consumption under ~100 KiB and requiring 0 heap allocations (`malloc`).
* **Evaluation Metric**: All researched techniques are evaluated strictly on whether they perform **"Removal (삭제)"** (eliminating 65816 instruction fetch/decode entirely) or **"Check (검사)"** (dispatch lookup test).

---

## 2. Precedent (1): `SNESRecomp` / `SuperMarioWorldRecomp` (by `mstan`)

* **Repository & File Path**:
  * Repository: [`mstan/snesrecomp`](https://github.com/mstan/snesrecomp) & [`mstan/SuperMarioWorldRecomp`](https://github.com/mstan/SuperMarioWorldRecomp)
  * Files: [`src/snesrecomp/recompiler.cpp`](https://github.com/mstan/snesrecomp/blob/main/src/snesrecomp/recompiler.cpp)
* **Mechanism**:
  * Translates 65816 opcodes into C functions ahead-of-time (AOT).
  * Direct branches / jumps are linked as direct C function calls between basic blocks.
  * Indirect jumps / dynamic branches are handled via synthesized C `switch`-statement dispatch (`_emit_dispatch`).
  * On lookup miss, returns `NORMAL` state, unwinding the host C execution stack cleanly.
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal of Fetch/Decode)** for direct basic blocks.
  * **검사 (Check)** via C `switch` statement for indirect dynamic branches.
* **Mapping to our Codebase**:
  * Maps to `tools/sfc_recomp/` (translator engine) and [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1) / [`cpu.c:147`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/cpu.c#L147).
* **RAM Cost**: On modern OS targets, 0 lookup table RAM (uses host compiler code generation and native OS executable sections). On embedded bare-metal, requires a PC dispatch index.

---

## 3. Precedent (2): `rc_dispatch` (Our Embedded Static Recompiler for SMW)

* **Repository & File Path**:
  * Submodule: [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1) & [`rc_dispatch.h`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.h#L1)
  * Integration: [`Core/Src/porting/snes/rc_smw_sites.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/Core/Src/porting/snes/rc_smw_sites.c#L1)
  * Documentation: [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L1)
* **Mechanism**:
  * Translates 8,371 65816 basic block sites into C site functions executed via External QSPI Flash XIP (`rc_smw.xip`).
  * **Per-Bank Knuth Multiplicative Open-Addressing Hash Dispatch**:
    * Hash function: `(pc * 2654435761u) & mask` with linear probing.
    * Load Factor maintained at $\approx 0.5$ (`sz = next_pow2(count * 2)`).
  * **Zero-Heap Allocation**: The ~85 KiB hash table (8,371 entries across 7 banks) is allocated statically in `RAM_EMU` overlay BSS, eliminating runtime `malloc` calls and DTCM OOM crashes.
  * Lookup takes ~6–8 cycles ($O(1)$ deterministic execution time), reducing rig instruction count by **−42.3%** vs. interpreter (4.57M vs 7.92M insn/frame).
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal of Instruction Fetch & Decode)**: Replaces interpreter loop decoding with native C site functions.
  * **검사 (Check)**: Performs 1 hash lookup (`rc_dispatch_lookup`) per site entry.
* **Mapping to our Codebase**:
  * [`external/sm/src/snes/rc_dispatch.c:26-67`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L26-L67) (`rc_dispatch_init`)
  * [`external/sm/src/snes/rc_dispatch.c:77-88`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L77-L88) (`rc_dispatch_lookup`)
* **RAM Cost**: **85.0 KiB** in `RAM_EMU` overlay BSS (0 DTCM heap allocation).

---

## 4. Precedent (3): `N64Recomp` (by `Wiseguy`)

* **Repository & File Path**:
  * Repository: [`Wiseguy/N64Recomp`](https://github.com/Wiseguy/N64Recomp)
  * Files: Generated `lookup.cpp` / [`src/recompiler/recompiler.cpp`](https://github.com/Wiseguy/N64Recomp/blob/main/src/recompiler/recompiler.cpp)
* **Mechanism**:
  * Translates MIPS MIPS-III instructions to native C functions.
  * Known jump tables (e.g. compiler-generated switch statements for IDO/GCC 2.7) are detected during static analysis and compiled into native C `switch-case` constructs.
  * Dynamic indirect jumps (`jalr`) use `LOOKUP_FUNC(pc)` calls, referencing an auto-generated function lookup table in `lookup.cpp`.
* **Classification ("삭제인가 검사인가")**:
  * **삭제 (Removal)** for direct function calls and branches.
  * **검사 (Check)** for indirect `LOOKUP_FUNC` table lookups.
* **Mapping to our Codebase**:
  * [`external/sm/src/snes/rc_dispatch.c:77`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L77) (`rc_dispatch_lookup`)
* **RAM Cost**: Scalable function pointer lookup array (proportional to total number of functions, typically ~100 KiB – 500 KiB on PC/console).

---

## 5. Precedent (4): Binary Search / Sorted Array PC Lookup (Alternative Embedded Strategy)

* **Repository & File Path**:
  * Concept analyzed in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L40)
* **Mechanism**:
  * Stores sorted `(pc, fn_id)` tuples per bank in RAM/Flash. Performs binary search (`bsearch` / `std::lower_bound`) on PC lookup.
* **Classification ("삭제인가 검사인가")**:
  * **검사 (Check)** ($\log_2(N)$ iterations per lookup).
* **Resource & Trade-off Evaluation**:
  * Memory Cost: ~33 KiB RAM (smallest RAM footprint).
  * **Evaluation on Cortex-M7**: Bank 0 with 3,767 sites requires 11–12 comparison iterations per lookup with high branch misprediction rates, destroying interpreter speedup savings (evaluated and rejected in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L40)).
* **RAM Cost**: **~33 KiB RAM**.

---

## 6. Precedent (5): 2-Level Page Table Dispatch (Alternative Embedded Strategy)

* **Repository & File Path**:
  * Concept analyzed in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L43)
* **Mechanism**:
  * Uses L1 bank page table + L2 256-byte page pointer table to map PC to site ID.
* **Classification ("삭제인가 검사인가")**:
  * **검사 (Check)** (2-level memory dereference).
* **Resource & Trade-off Evaluation**:
  * Memory Cost: **102.5 KiB RAM** (198 pages $\times$ 256 entries $\times$ 2 bytes).
  * **Evaluation**: Requires 2 sequential memory reads and consumes MORE RAM (102.5 KiB) than Knuth multiplicative hash (85 KiB). (Evaluated and rejected in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L43)).
* **RAM Cost**: **~102.5 KiB RAM**.

---

## 7. Summary Comparison Matrix

| Project / Strategy | Repository & File | PC Dispatch Mechanism | Removal vs. Check | Our Mapping | RAM Cost |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **1. SNESRecomp** | [`mstan/snesrecomp`](https://github.com/mstan/snesrecomp)<br>`src/snesrecomp/recompiler.cpp` | Synthesized C `switch` statement for indirect branches | **삭제** (Direct basic blocks)<br>**검사** (Indirect C `switch`) | `tools/sfc_recomp/`<br>[`rc_dispatch.c:147`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L147) | 0 B (PC OS)<br>Scalable |
| **2. rc_dispatch (Our Core)** | [`external/sm/src/snes/rc_dispatch.c`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L1)<br>`rc_dispatch.c` | Per-bank Knuth Multiplicative Open-Addressing Hash ($\text{LF} \approx 0.5$) | **삭제** (65816 Fetch/Decode)<br>**검사** (1 Hash lookup/site) | [`rc_dispatch.c:26`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L26)<br>[`rc_dispatch.c:77`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L77) | **85.0 KiB**<br>(Overlay BSS, 0 Heap) |
| **3. N64Recomp** | [`Wiseguy/N64Recomp`](https://github.com/Wiseguy/N64Recomp)<br>Generated `lookup.cpp` | Static `switch` for jump tables + `LOOKUP_FUNC` array | **삭제** (Direct MIPS branches)<br>**검사** (`LOOKUP_FUNC` table) | [`rc_dispatch.c:77`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/external/sm/src/snes/rc_dispatch.c#L77) | ~100–500 KiB |
| **4. Sorted Binary Search** | Analyzed in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L40) | `bsearch` / `std::lower_bound` on sorted PC tuples | **검사** ($\log_2 N$ loop comparisons) | Rejected alternative | ~33 KiB |
| **5. 2-Level Page Table** | Analyzed in [`docs/RC_DISPATCH_ANALYSIS.md`](file:///home/ubuntu/app/jupyterLab/notebooks/game-and-watch-retro-go-sd/docs/RC_DISPATCH_ANALYSIS.md#L43) | L1 Bank Table + L2 Page Pointer Array | **검사** (2-Level memory dereference) | Rejected alternative | ~102.5 KiB |
