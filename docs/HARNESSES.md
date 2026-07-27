# Harnesses — what proves what, and how to run it

Every harness in this tree exists because something shipped broken while a
different check was green. The one rule they all serve: **a harness must be the
same program as the firmware** — same source list, same defines, real files,
never a reimplementation. Three Super Metroid releases and a 0%-covered JPEG
driver were the tuition for that rule (CLAUDE.md tells both stories).

The device is still the final judge. A harness that cannot reproduce a device
fault is a harness with a limit, not proof the fault is imaginary.

## The suite

| | |
|---|---|
| run | `tests/run.sh` (plain gcc, no ARM toolchain needed; ELF-inspection tests SKIP without one) |
| coverage | `tests/coverage.sh`, denominator in `tests/coverage_scope.txt` — the code we own and can run, as data, with a reason per line |

The suite compiles the real firmware files against fakes of the hardware
(`tests/*_stubs/`, a fake flash that ANDs bits like NOR flash, a real FatFs on
a RAM disk). Wiring tests (`test_idle_timeout_wired.sh`,
`test_gba_m4a_wired.sh`) deserve special mention: they assert that callers
call — this repo's bugs live in the function nobody wired, not the function
that was wrong.

## Per-core host harnesses — `linux/`

One `Makefile.<core>` per system, sharing the `odroid_*.c` shims in the same
directory. Builds the core's overlay sources into a desktop binary with SDL
output. This is the everyday "does the core still boot and play" loop;
`update_<core>_rom.sh` refreshes the test ROM each expects.

Caveat that cost us: these run the core's *logic*, not the device's memory
map, alignment traps, or linker layout. For those, see the device-shaped
harnesses below.

`Makefile.wswan` is the exception in shape: headless (no SDL), and it runs a
**two-process** savestate round-trip — `record` snapshots in one process, then
`resume` in a FRESH process is a real device COLD boot (every static at its
reset value, not warmed by a prior pass). A single process carries statics
across passes and hides cold-boot-only bugs — this rig is what reproduced, on
the host, the One Piece Grand Battle resume hang (MemDummy scratch the save
never captured) that only ever showed on hardware. `cross` mode prints a
RUNHASH that must equal the m7 rig's. Pin `time()` (it does) so RTC-reading
games gate reproducibly.

## Device-shaped harnesses — `tools/`

### `tools/sm_harness` — Super Metroid, the way the device runs it
- `device_run.sh <rom> [frames]` — compiles the core **from the Makefile's own
  source list** with the device's defines (`-DTARGET_GNW`), shims the firmware
  allocators, and forces the device CPU's rules on the host:
  `-fsanitize=alignment` (M7 traps 64-bit unaligned) and
  `-Werror=implicit-function-declaration`. Reverting any shipped fix
  reproduces its fault here.
- `device_parity.sh` (in the suite) — links exactly what the device links, so
  a symbol the firmware would silently resolve into another core's overlay is
  an error here.

### `tools/gba_harness` — gpSP's load path on an honest address space
- `run.sh` — the cart-load path with `ROM_BUFFER_SIZE=0` (XIP: the cart is
  never buffered). QEMU maps address 0 and shrugs; a hosted OS SIGSEGVs, which
  is what the device's bus fault actually meant. `--red` rebuilds the pre-fix
  file from git history and shows it crash.
- `host_stubs.c` is the shared firmware-allocator shim the other GBA tools
  link.

### `tools/gba_m4a` — the M4A mixer HLE, and the GBA investigation rig
- `prove.sh [--blocks|--e2e|--speed] <rom> [frames]` — the full proof: every
  hooked block run both ways and compared to the register, flag, cycle and
  byte (`--blocks`); the whole game run hook-off vs hook-on and hashed —
  screen, audio, RAM, IO **and the clock** — every frame (`--e2e`, which
  first runs hook-off against itself, because Emerald's RTC taught us a
  comparator must agree with itself before it may compare); and a sabotaged
  build that must fail (RED).
- `census.py` — counts mixers straight out of a ROM corpus, no emulation.
- `prove_main.c` has grown into the general GBA rig: `M4A_AUDIO_RAW=` (audio
  tap), `IDLE_PC=`/`IDLE_COND=ne` (idle-skip A/B on the device's own
  semantics — mGBA cannot: forcing its remover disables its detector),
  `IDLE_TRACE=1` (per-frame pc/halt/VCOUNT/interrupt state), `NO_KEYS=1`,
  `RATE_TRACE=1`. The full knob table with the investigation each knob earned
  its keep in: `tools/gba_m4a/README.md`. The playbook that says which rig
  answers which GBA question: `Core/Src/porting/gba/CLAUDE.md`.

### `tools/m7_qemu_rig` — executed-instruction counts on a real ARMv7-M stream
The GBA feasibility study's QEMU trick, rebuilt as a tool this time. QEMU's
mps2-an500 (Cortex-M7) with `-icount shift=0` makes virtual time tick exactly
1 ns per executed instruction; the board's CMSDK timer runs on virtual time,
so a timer delta IS an instruction count (the rig calibrates the exact scale
on itself at boot — 40.000 insn/tick — and prints it).
- The 32X core was removed on 0727 (`docs/32X_CLOSED.md`), and its rigs with
  it. The measurement workflow those rigs implemented -- histogram, hot loop,
  on-device DWT -- is still worth reading as a method:
  `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md` and
  `docs/32X_DEVICE_MEASUREMENT_LOG.md`.
- `run_vb.sh <rom.vb> [frames]` — the linux/vb harness driver, bare-metal:
  same core sources, same input script, so its frame hashes must equal the
  host harness's for the same ROM (they do — that's the cross-validation).
  Prints per-window `emu=`/`blit=` instructions/frame.
- What it answers: instructions-per-frame A/B of algorithmic changes, and
  budget math against the CPU clock, in the device's OWN ISA. What it cannot
  answer: caches and wait states (QEMU models neither) — absolute fps still
  belongs to the device ledger. First result: the VB eye-skip + spin-credit
  pair measured **emu −75.9%, total −54.2%** insn/frame on Wario Land, with
  identical RUNHASH old-vs-new — numbers the x86 host undersold by 3x.
- `run_wswan.sh <rom.ws> [frames]` — the WonderSwan (oswan) variant, split
  three ways: CPU / PPU / blit. It builds twice (render on/off) and subtracts,
  because the PPU is per-scanline inside WsRun. A dedicated `mps2_an500_ws.ld`
  XIPs the up-to-16MB ROM in PSRAM. `profile_ws_layers.sh` attributes the PPU
  cost across BG/FG/sprite (via a non-const `Layer[]`). First result: One Piece
  Grand Battle's battle frame is PPU-bound (2.2M insn), and the WSRender tile
  rewrite cut it ~32% at identical RUNHASH.
- `rig_runtime.c`/`mps2_an500.ld` are core-agnostic: copy `rig_vb.c`'s shape
  to put any other core on the same scale.

### `tools/jpeg_harness` — the HW JPEG driver against a fake HAL
- `run.sh` — compiles `hw_jpeg_decoder.c` itself (its three previous tests
  reimplemented the HAL and covered 0% of it) against `hal_fake/`, including
  the lock-poisoning and input-end-callback cases that killed two releases.
  RED comes from `git show <fix>^:` — the actual pre-fix file must fail.

### `tools/snes_harness` — closed initiative, kept for the record
The SNES-emulation feasibility rig (verdict: ⛔ the PPU alone is 14 ms of a
16.6 ms frame; do not reopen). Kept because it is the working example of the
event-scheduler experiment and boots zelda3/SMW against the shared core.

## On-device instrumentation — branch `feat/gba-probe`

The probe that measured what QEMU could not: guest-PC histogram, DWT cycle
counters, and the frame-budget breakdown that told *wait* from *work* (the
distinction that saved two builds — a number that grows when everything else
gets faster is a wait, not a cost). Not on `testbed`; cherry-pick it onto a
work branch when a core needs to answer "what is it actually doing" on the
hardware itself.

## On-device instrumentation — `SNES_DEVICE_PROFILE=1`

The SNES answer to the same question, and it lives on `testbed` rather than a
side branch: a 3-ledger frame profiler that dumps to `/snes_dwt.txt` after 64
frames. **DWT active cycles** (top-level disjoint, IRQ-inclusive) + **exclusive
PPU/APU inside `run_frame_events`** (via a generated copy of `external/sm`'s
`snes.c`) + a **sleep-safe TIM2 wall clock and audio-deadline histogram** —
because the pacing wait is `__WFI()` and `DWT_CYCCNT` stops in sleep, so the
one bucket that decides "compute-bound or deadline-bound" cannot be measured
with DWT at all. Every gate (nesting, monotonicity, residual, probe cost, IRQ
share, wall-vs-hardware) prints in the dump, and
`scripts/check_snes_profile_wired.sh` proves on every link that the probes are
in the binary. Full guide: [SNES_DEVICE_DWT.md](SNES_DEVICE_DWT.md).

## Choosing

| question | harness |
|---|---|
| does the core still boot/play after my change | `linux/Makefile.<core>` |
| will it survive the device's CPU and memory map | `tools/sm_harness/device_run.sh` (copy its pattern for a new core) |
| does the firmware link the same program I tested | `device_parity.sh`, `scripts/check_core_symbol_aliases.py` |
| is my HLE/optimization bit-exact, including time | `tools/gba_m4a/prove.sh` (the model to copy) |
| is the thing actually *wired* | a `tests/test_*_wired.sh` — write one |
| what is the device really spending a frame on | `feat/gba-probe`; for SNES, `SNES_DEVICE_PROFILE=1` |
| is fps compute-bound or sitting on the audio deadline | `SNES_DEVICE_PROFILE=1` Ledger C — DWT alone cannot answer this, it goes blind in `__WFI()` |

## `tools/gnw_hw_harness/` — the device memory-budget contract (the missing gate)

Built 2026-07-19 after two builds shipped that passed docker (link-only) and
the QEMU rig (flat, huge RAM) but **died on real hardware**: rc's dispatch
table OOM'd the DTCM heap, and a 32X static-AHB section overran the Draw2FB
pool. Neither the rig nor docker models the constrained runtime memory, so
this closes that gap. Three tiers:

1. **Static budgets (Tier 1).** Extracts every region's real size from the
   `.map`/ELF of a canonical-flags build — never from docs (the docs said
   480 MHz; the firmware PLL is 280/312/340). Link-time `ASSERT`s fail the
   build when an overlay/BSS/pool is exceeded.
2. **Effective-free caps (Tier 2).** The trap that got rc: the DTCM heap is
   82,944 B but the launcher has already taken 74,728 B at emu-init, so a
   core has only **8,216 B** free — not 81 KB. The harness caps allocations
   at the *resident-subtracted effective free* (from a bound device profile,
   never hardcoded, so a flag change can't silently pass), with a 0xAA-poison
   allocator (calloc zeroes) to surface uninitialised-field bugs the host
   would otherwise hide as zero.
3. **DWT-calibrated timing oracle (Tier 3).** QEMU counts instructions;
   real-device DWT traces calibrate the cost model; fps is predicted with a
   stated error band. Host-hash == QEMU-hash == device-hash for correctness.

It is **not** a full STM32H7 peripheral/cycle sim (a custom QEMU board was
deliberately deferred — months of work, still approximate). It's a budget
contract. Pinned GCC 15.2.1 + QEMU 8.2.2 container (`container.sh`).

**Run it before committing any new `malloc`/`ahb_malloc`/`itc_malloc` or
static section:**
```
tools/gnw_hw_harness/run.sh --map build/gw_retro_go.map \
  --profile <device-profile.json> \
  --proposal dtcm:61440:rc_dispatch     # FAILs against 8,216 B effective free
  --proposal ahb:86016:Draw2FB
tools/gnw_hw_harness/run.sh --tests      # reproduces both 2026-07-19 regressions
```
Its regression tests (`test_old_32x_ahb_layout_reproduces_overflow`, the rc
DTCM OOM, `tests/test_rc_dispatch_heap.c`) reproduce today's two device-only
faults on the host — proof the class is now caught before a flash.

### Choosing (add to the table above)

| question | harness |
|---|---|
| will my new allocation fit the device's real (resident-subtracted) memory | `tools/gnw_hw_harness/run.sh --proposal <region>:<bytes>:<label>` |
