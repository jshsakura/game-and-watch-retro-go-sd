# Capcom Play System 1 (CPS-1) — Feasibility & Port Plan

> Status: **feasibility study only, design phase** (2026-07-20).
> Verdict: **TECHNICALLY FEASIBLE. Memory constraint (740KB) is bypassed via OctoSPI Flash XIP + Dynamic Tile Caching. Universal cores (MAME/FBA) are REJECTED due to linker limits and heap fragmentation; ONLY a decoupled Standalone CPS-1 Core is feasible.**

---

## 1. Why CPS-1 on Game & Watch?
Users demand high-fidelity beat-'em-ups like *Tenchi wo Kurau II (Warriors of Fate)* or *Final Fight*. High-level 16-bit arcade emulation is highly attractive but extremely challenging under a **740KB RAM** ceiling.

This plan details how to fit CPS-1 into the device's strict budget, referencing the optimization patterns proved in GBA (gpSP) and Sega 32X (PicoDrive) ports.

---

## 2. Memory Budget Allocation (740KB RAM)

A standalone CPS-1 core requires exactly the following hardware structures:

| Memory Pool | Target Allocation | Physical RAM Mapping (STM32H7) | Notes |
|---|---|---|---|
| **68000 WRAM** | 64 KB | **DTCM** (Data Tightly-Coupled Memory) | Fast RAM, zero CPU wait state |
| **Z80 RAM** | 2 KB | DTCM / SRAM1 | Trivial |
| **VRAM** | 192 KB | **SRAM1/2** (AXI SRAM) | Video buffers & scroll layers |
| **Palette / OAM** | 12 KB | DTCM / SRAM1 | Sprite list and color palettes |
| **Emu Engine State** | ~150 KB | SRAM1/2 | Static struct variables (No `malloc`) |
| **Dynamic Tile Cache**| **256 KB** | **SRAM1/2 / DTCM remainder** | LRU tile buffer for sprite graphics |
| **Overlay Stack/Heap**| ~64 KB | SRAM3 | Reserved for stack and scratchpad |
| **Total Resident** | **~740 KB** | | **Fits exactly inside the G&W RAM envelope** |

---

## 3. High-Performance Core Architecture (3 Core Strategies)

### Strategy 1: Program ROM (68000 PRG) ➔ OctoSPI Flash XIP (Memory-Mapped)
* **Problem**: 68000 program code ranges from 1MB to 1.5MB. We cannot load it to RAM.
* **Solution**: Mount the 68000 PRG-ROM partition of the SD/ExtFlash using STM32's **OctoSPI Memory-Mapped Mode** at address `0x90000000` (or similar).
* **Execution**: Modify the 68000 emulator core (preferably an optimized C interpreter or Cyclone 68k) to fetch opcodes directly from this mapped address space. The STM32 L1 instruction cache will mitigate Flash read latency. **RAM Cost = 0 bytes**.

### Strategy 2: Graphics ROM (CHR) ➔ 256KB LRU Sprite Tile Cache
* **Problem**: CPS-1 graphics ROMs are 2MB to 4MB in size. Dynamic rendering directly from Flash is too slow.
* **Solution**:
  - Dedicate **256 KB** of the resident RAM to act as a **Dynamic Sprite/Tile Cache**.
  - Before rendering a frame, pre-scan the CPS-1 Object RAM (OAM) to identify which Sprite/Tile IDs are active in the current viewport.
  - If a tile is cached, draw it directly. If a cache miss occurs, fetch the raw tile data from OctoSPI Flash, decode it to native RGB565 (or 8bpp index), and store it in the cache using a **Least Recently Used (LRU)** replacement policy.
  - Since beat-'em-ups are side-scrollers, the active sprite set changes slowly, ensuring a high cache hit rate (>=80%).

### Strategy 3: Critical Loops to ITCM & RAM Placement
* **DTCM Allocation**: Force the 64KB `cps1_wram` and emulator state variables into **DTCM** via linker attributes (e.g., `__attribute__((section(".dtcm")))`).
* **ITCM Allocation**: Copy the time-critical interpreter loops, `render_sprites()`, and `render_bg()` blitters into the **64KB ITCM** (Instruction Tightly-Coupled Memory) during core boot. This avoids flash wait cycles completely during the rendering loop.
* **DWT Profiling**: Embed DWT (Data Watchpoint and Trace) cycle counting directly in the video blitters to measure and trim cycle budgets.

---

## 4. Integration Blueprint (Freestanding C Port)

To bypass the complex, bloated libretro wrapper of FB Alpha, the port must extract only the minimum required components and build them as a custom freestanding C overlay.

### Files to create:
* `Core/Src/porting/cps1/main_cps1.c` - Entrypoint `app_main_cps1()`, input loops, VRAM-to-LCD blitter.
* `Core/Src/porting/cps1/cps1_core.c` - VDP rendering logic, palette decoding, sprite cache manager.
* `Core/Src/porting/cps1/cpu/` - Optimized standalone 68000 & Z80 emulator cores.
* `Core/Src/porting/cps1/sound/` - Standalone YM2151 & OKIM6295 emulation (with flash-streaming ADPCM mixer).

### Files to edit:
* `Core/Src/retro-go/rg_emulators.c` - Register CPS-1 emulator (`add_emulator("CPS-1", "cps1", "zip bin", ...)`).
* `Makefile.common` / `Makefile` - Wire source paths, enable compilation of `cps1.bin` overlay core.
* Linker script overlays (`STM32H7B0VBTx_SDCARD.ld`) - Allocate `.overlay_cps1` memory space.

---

## 5. Summary Recommendation for Tmux / Opus Developer
1. **Memory feasibility is verified**. Do not attempt to run a generic MAME/FBA package; it will crash due to heap fragmentation.
2. Port only the **CPS-1 specific standalone core** using the proposed static memory layout.
3. Start by verifying boot on the PC/SDL host harness (`linux/Makefile.cps1`) with a lightweight 1.5MB ROM (e.g., *Dynasty Wars / Tenchi wo Kurau I*) before flashing the G&W target.

---

## 6. Phase 1+2 status (this branch)

Everything above is a design projection, not a verified result -- "memory
feasibility is verified" means the arithmetic in section 2 adds up to 740KB,
not that any allocation exists yet. What actually exists on
`explore/cps1-feasibility` right now:

- **Phase 1**: this branch, in its own worktree, isolated from `main` and
  every other core.
- **Phase 2**: a shared, freestanding stub core
  (`Core/Src/porting/cps1/cps1_core.{h,c}`) compiled unmodified by two
  harnesses -- no 68000/Z80/PPU/sound emulation exists yet, only the
  plumbing each will plug into:
  - `linux/Makefile.cps1` + `linux/cps1/main.c` -- headless x86 build,
    `--engine=interpreter|recompiler|diff` (diff is default: runs both
    "engines" every frame and fails loudly on any checksum mismatch),
    `--dump-ppm` for a visual sanity check. Both engines currently run
    identical logic, so a passing diff proves the harness compares
    correctly, not that emulation is correct.
  - `tools/m7_qemu_rig/rig_cps1.c` + `run_cps1.sh` -- the same stub built
    hard-float and run as a real ARMv7-M instruction stream under QEMU's
    mps2-an500 (`rig_runtime.c`/`mps2_an500.ld` reused verbatim from the
    Virtual Boy rig -- both are core-agnostic). Reports instructions/frame
    on the device's own ISA, which is the closer-to-hardware number this
    initiative needs before trusting any of section 2/3's percentages --
    QEMU still can't model caches or flash wait states, so it bounds
    instruction count, not fps.

```
cd linux && make -f Makefile.cps1
./build/retro-go-cps1 --frames=600 --engine=diff --dump-ppm

cd .. && bash tools/m7_qemu_rig/run_cps1.sh 600
```

**Not yet done, and not safe to assume**: any real 68000/Z80 interpreter,
any PPU/sprite rendering, any static recompiler, any ROM loading format
(CPS-1 ships as multiple PRG/GFX/audio ROM files per game -- host layout
undecided), YM2151/OKI6295 audio in any form, and every hardware-fidelity
claim in section 3 of `CPS1_SENIOR_TRICKS_ANALYSIS.md` (DMA2D dual-layer,
LTDC CLUT, OctoSPI XIP execution). Those are the next milestones, each of
which should get its own RED-before-GREEN proof the way
`tools/sm_harness`/`tools/gba_m4a` did for their cores, not a port of the
whole claim at once.
