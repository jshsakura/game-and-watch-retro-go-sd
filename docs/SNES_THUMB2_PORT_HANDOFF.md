# SNES Thumb-2 65816 port — implementation handoff

## Roles and scope

- Implementer: GLM session.
- Adversarial reviewer: AGY session. Review only; do not edit implementation.
- Supervisor/integration gate: Codex session.
- Worktree: `/home/ubuntu/app/jupyterLab/notebooks/gnw-dwt-thumb2`
- Branch: `exp/snes-thumb2-core`
- Base: `249914d8a37c98e77ce57b401b3bcadc52e649b7`
- Repository changes must stay in this worktree/branch.

The goal is a Cortex-M7 Thumb-2 65816 execution engine that breaks the current
CPU ceiling. It may be implemented either inside LakeSnes or as a separate,
self-contained CPU emulator with a thin LakeSnes bus/event adapter. The latter
is explicitly allowed if it produces a cleaner register-cached/block-running
design. This is a source-guided semantic port of the DrPocketSNES ARM core
design, not an attempt to assemble A32 source on Cortex-M7.

A separate CPU engine does not have to preserve the internal layout of
LakeSnes `Cpu`, but its adapter must preserve externally visible SNES state,
bus side effects, interrupt timing, DMA/event boundaries, save/load behavior,
and hashes. Replacing more of the emulator is allowed when measurements justify
it; compatibility gates are not waived.

The user has stated that the resulting firmware is for non-commercial use.
Retain all applicable source notices and record provenance. Do not relabel
DrPocketSNES/Snes9x-derived material as MIT.

## Reference source and correction to the earlier GLM report

Reference checkout:

- repository: `https://github.com/Apaczer/DrPocketSNES`
- branch: `miyoo`
- inspected commit: `0eef0a64828fa185fec67a6cfb712a9101650894`

The earlier `/tmp/glm_armsnes_asm.md` file inventory is wrong. The 65816 ARM
assembly does exist in all three upstream branches. The active source set is:

- `src/os9x_65c816_spcasm.s`
- `src/os9x_65c816_common.s`
- `src/os9x_65c816_opcodes.s`
- `src/os9x_65c816_global.s`
- `src/os9x_65c816_mac_gen.h`
- `src/os9x_65c816_mac_mem.h`
- `src/os9x_65c816_mac_op.h`
- `src/os9x_asm_cpu.cpp`

The upstream Makefile adds `os9x_65c816_global.o`,
`os9x_65c816_spcasm.o`, `os9x_65c816_spcc.o`, and `os9x_asm_cpu.o`; with
`ASMCPU`, `cpuexec.cpp` calls `asmMainLoop_spcAsm(&CPU)` or
`asmMainLoop_spcC(&CPU)`.

This control experiment has already been run:

```text
arm-none-eabi-gcc -mcpu=arm926ej-s -marm \
  -c os9x_65c816_spcasm.s
    -> success, 394,344-byte .data/code section

arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb \
  -c os9x_65c816_spcasm.s
    -> fails: A32 conditional execution/register/addressing forms
```

Treat the A32 source as a semantic and structural reference. Do not mechanically
copy it into the product build.

## Contracts that must not change

Current CPU state and entry contract:

- `external/sm/src/snes/cpu.h:14-47` — `Cpu`.
- `external/sm/src/snes/cpu.c:140-171` — one opcode/interrupt per call,
  returns `cyclesUsed`.
- `external/sm/src/snes/snes.c:592-644` — CPU bus access also charges
  `cpuMemOps` and `cpuCyclesLeft`.
- `Core/Src/porting/snes/main_snes.c:139-203` — the event loop converts
  opcode cycles to master dots and interleaves DMA/APU/PPU.

The lowest-risk integration may initially preserve the exact public contract:

```c
int cpu_runOpcode(Cpu *cpu);
```

Do not blindly import DrPocketSNES's `S9xDoHBlankProcessing`, APU loop,
`SCPUState`, `Memory.Map[]`, or frame loop. A separate engine may define its
own compact state and run loop, but it must use an explicit adapter and must
not consume beyond an event boundary.

## Implementation sequence

### Stage 0 — guardrails and reproducible build

1. Add a build flag such as `SNES_THUMB2_CPU`, default `0`.
2. Flag-off builds must be byte/behavior neutral.
3. Add a SNES-specific `.S -> .o` rule; adding `.S` to `SNES_C_SOURCES` is
   not sufficient because the current object transform only handles `.c`.
4. Generate or compile-check every assembly-visible `Cpu`/`Snes` offset.
   Hard-coded offsets without `_Static_assert` or a generated offsets include
   are forbidden.
5. Keep the original C implementation callable as the oracle/fallback. Use a
   clear symbol such as `cpu_runOpcode_c`; avoid recursive wrapper aliases.

### Stage 1 — semantic bridge

Create a Thumb-2 source in unified syntax. At this stage C may retain
interrupt/WAI/STP handling and opcode fetch, then call assembly with the fetched
opcode. Implement a coherent first family:

- flag operations: CLC, SEC, CLI, SEI, CLV, CLD, SED;
- NOP;
- register transfers: TAX/TAY/TXA/TYA/TSX/TXS/TCD/TDC/TCS/TSC;
- register increments/decrements and XBA.

Correctly honor M, X, and E width semantics. Unsupported opcodes must take the
unmodified C path without double fetches, double bus charging, or changed
interrupt ordering.

This stage proves ABI and semantics. It is not a performance result.

### Stage 2 — assembly fetch and dispatch

Move the common opcode fetch and table dispatch into assembly after Stage 1
passes. Use Thumb targets with bit 0 set and normal AAPCS calls. Replace A32
idioms deliberately:

- broad conditional execution -> short `IT` where valid or explicit branches;
- `STMFD {...,PC}` call tricks -> `BL`/`BLX`;
- A32 PC jump tables -> Thumb-safe word table or `TBB`/`TBH`;
- A32 use of `r14` as persistent state -> a callee-saved register allocation.

Implement direct fast paths only for side-effect-free ROM/WRAM cases. MMIO,
SRAM, DMA, B-bus, and mapper edge cases must retain `snes_cpuRead/Write`.

### Stage 3 — generated opcode families

Port the A32 design at the macro/semantic-family level rather than hand-copying
five complete opcode tables. Share mode-independent handlers and generate only
the required M/X-width variants. Keep hot dispatch/handlers in ITCM and cold
handlers outside it.

The existing SNES ITCM section already contains `cpu.o` and `ppu.o`; the 64 KiB
linker ASSERT is a hard gate. Report `.text/.rodata` size after every family.

### Stage 4 — optional bounded runner

Only after single-opcode parity, evaluate:

```c
int cpu_runUntil(Cpu *cpu, int master_dot_budget);
```

Its purpose is to retain 65816 state in M7 registers across multiple opcodes.
It must return before DMA/HDMA, IRQ/NMI, WAI/STP, MMIO-triggered synchronous
work, or the next PPU/APU event boundary. Do not replace the scheduler on
intuition; demonstrate hash parity first.

## Differential verification

For every implemented opcode and every relevant M/X/E combination:

1. Start C and Thumb-2 executions from identical randomized CPU/bus state.
2. Compare all CPU registers and flags, PC/PB/DB/DP/SP, waiting/stopped state,
   `cyclesUsed`, `cpuMemOps`, `cpuCyclesLeft`, and interrupt latches.
3. Compare ordered bus-read/write traces including address and value.
4. Exercise page/bank wrapping, direct-page low-byte penalties, stack wrapping,
   decimal mode, branch page crossing, IRQ/NMI priority, and WAI wakeup.

The host cannot execute Thumb-2. Use the existing Cortex-M7 QEMU rig or add a
minimal semihosted M7 differential target. A test that reimplements opcode
semantics independently is not an oracle; it must link the real
`external/sm/src/snes/cpu.c` reference path and the real assembly object.

Full-emulator gates:

- profiler off;
- Zelda representative scenes, not one boot scene;
- state hash, audio hash, and framebuffer hash identical;
- save/load continuity;
- DMA/HDMA and IRQ-heavy ROM coverage;
- flag-off regression suite unchanged.

## Performance and product gates

Do not infer device FPS from QEMU instruction count.

The user's product target is a sustained gain of approximately 10 FPS, with a
20% whole-frame improvement as the preferred success criterion. It must persist
across representative gameplay rather than appearing only in a boot/menu
scene. With the measured `cpu_only = 45.4%`, a CPU-only speedup of about 1.6x
would yield about 20% whole-frame improvement if all other buckets remain
unchanged; measure spillover rather than relying on this Amdahl estimate.

The implementation remains experimental until a paired real-device A/B shows:

- at least 1.7x reduction in the `cpu_only` bucket, or enough measured
  whole-frame gain to close the actual 60 FPS gap;
- no increase that cancels the gain in scheduler, bus, DMA, APU, or PPU;
- profiler-off result reproduced across several Zelda scenes and at least two
  other LoROM/HiROM games;
- state/audio/framebuffer hashes remain identical.

If the first complete hot family cannot plausibly reach this threshold, stop
before porting the full opcode matrix and report NO-GO with measurements.

## Commit and reporting protocol

- Make small, reviewable commits: build/offsets, bridge, opcode family, tests,
  fetch/dispatch, performance.
- Do not rewrite or squash commits while AGY is reviewing them.
- After each milestone, update `/tmp/glm_thumb2_status.md` with commit hash,
  tests, code size, known gaps, and the next action.
- Do not modify `/tmp/agy_thumb2_review.md`; that file belongs to AGY.
