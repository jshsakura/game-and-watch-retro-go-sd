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
- The end-to-end 32X histogram, hot-loop, correctness, and on-device DWT
  workflow is in `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md`.
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

## Choosing

| question | harness |
|---|---|
| does the core still boot/play after my change | `linux/Makefile.<core>` |
| will it survive the device's CPU and memory map | `tools/sm_harness/device_run.sh` (copy its pattern for a new core) |
| does the firmware link the same program I tested | `device_parity.sh`, `scripts/check_core_symbol_aliases.py` |
| is my HLE/optimization bit-exact, including time | `tools/gba_m4a/prove.sh` (the model to copy) |
| is the thing actually *wired* | a `tests/test_*_wired.sh` — write one |
| what is the device really spending a frame on | `feat/gba-probe` |
