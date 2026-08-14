# SNES — the fetch-page cache was refusing most of the library

## The headline

`snes_cpuRead`'s fetch-page cache was gated on `cart->romMask`, which
`cart_setRomSize` sets **only for a power-of-two ROM**. A 3 MB cartridge — Super
Metroid, Final Fantasy VI, most 24 Mbit games — therefore had no cache at all,
and every opcode fetch walked `snes_read` → `cart_read` → `cart_readLorom`, four
calls and a full classification chain, three of them out of ITCM into
wait-stated RAM_EMU. Super Metroid took the slow path for **30,463 of its 34,314
bus reads a frame**.

A second class was excluded on purpose: `cart_attachDsp1()` cleared `romMask`
for the *whole* 24-bit bus so that sixteen banks of LoROM DSP window would
decode correctly.

Both are fixed by one table — `cart->bankBase[]`, built once at load with the
mapper and any non-power-of-two fold already baked in — so a page install is a
table load and an add. Device, play scenes resumed from savestates, bracketed
A/B/A/B:

| ROM | emulated fps | drawn fps |
|---|---|---|
| **Super Metroid** | 47.5 → **60.9** (+28%) | 11.9 → **22.4** (+88%) |
| **Pilotwings** | 44.9 → **59.1** (+32%) | 11.2 → **17.6** (+57%) |
| Zelda 3 | 60.9 → 61.0 | 20.4 → 21.5 |
| Mario Kart | 60.5 → 60.8 | unchanged |
| Dragon's Magic | 57.3 → 57.4 | 44.6 → 45.4 |

Metroid and Pilotwings now sit on the 60.15 audio-DMA cap. Before this they were
a quarter under it, with the stretcher pinned at its 0.839x floor and 70–83k
underruns in a window.

Gated on the rig at 1200 frames over 14 distinct cartridges, 32 KB to 4 MB,
LoROM and HiROM: **every STATEHASH and AUDIOHASH bit-identical**. Non-power-of-two
carts move −22% to −34% instructions a frame; everything else is within ±0.02%.

Three things that had to be got right, each of which cost a measurement:

- **It must not be a call.** An earlier version asked a helper for the folded
  base at the install site. Merely having that call inside `snes_cpuRead` cost
  **1.4% of A Link to the Past** — a cartridge that never takes the branch —
  because every caller-saved register is clobbered across it. The same lesson as
  the `memcpy` in EarthBound's scroll.
- **Ask once, at load.** A cart that cannot be page-cached must be decided in
  `cart_setRomSize`, not re-asked per read: the version that returned NULL from
  a helper cost **+1.8%** on such a dump.
- **The bank table needs 64 KB granularity**, not 8 KB, because a HiROM bank is
  64 KB and the fold has to be linear across it.

Knobs for the A/B arms: `SNES_ROMPAGE_FOLD=0`, `SNES_DSP_FASTPATH=0`.

---

# The profiles and the closed questions that led there

2026-08-14. Everything here is the device unless it says rig. Play scenes only:
every cartridge wrote its own slot-0 savestate with `GNW_AUTOSAVE_FRAME=1200`
and every profile below resumed it (`g_snes_state_resumed == 1` checked before
the number was kept).

The handoff (`docs/SNES_NEXT_SESSION.md`) ended with two things called
genuinely unmeasured. This closes the first one and starts on the second.

## The card now has DSP-1 cartridges on it

The core implements exactly one coprocessor, DSP-1 (`external/sm/src/snes/dsp1_hle.c`).
The header cannot tell DSP-1 from DSP-2/3/4 — all of them encode as romType
`0x03`/`0x05` with coprocessor nibble 0 — so a DSP-2 cart (Dungeon Master), a
DSP-3 (SD Gundam GX) or a DSP-4 (Top Gear 3000, Ballz 3D) will attach the DSP-1
HLE and misbehave quietly. Those were left off deliberately; see
`snes-dsp-family-protocol` in memory.

| index | file | chip | size |
|---|---|---|---|
| 1 | Super Metroid | — | 3 MB |
| 4 | Super Mario Kart | DSP-1 | 512 KB |
| 7 | `zz_pilotwings.smc` | DSP-1 | 512 KB |
| 8 | `zz_airdiver.smc` | DSP-1 | 512 KB |
| 9 | `zz_suzuka8h.smc` | DSP-1 | 1 MB |
| 10 | `zz_battleracers.smc` | DSP-1 | 1 MB |

The index is the position in `/roms/snes` in FatFs order with `._*` and
non-`smc/sfc/fig/swc` entries dropped — `emulator_add_rom_file` does not sort.
It was confirmed for each cart by the savestate filename that appeared in
`/data/snes` after the benchsave pass, not by counting.

All four booted, ran 1200 frames and saved. Nothing needed a firmware change.

## HDMA is not a lever — closed, with numbers from both instruments

The handoff listed "HDMA, 226 calls a frame on Kart (one per scanline; Mode 7
rewrites its perspective table every line). Nothing in this tree has ever priced
it."

It is priced now, and it is **0.3% of Kart's frame**.

**The device, Kart's play scene** (`prof` arm, Ledger B): `hdma [D] avg=33,410
cycles = 0.3%` of an 8,631,610-cycle drawn frame; `0.5%` on skipped frames.

**The rig says why**: Mario Kart activates **no HDMA channel at all**. Over 6000
frames `dma_cycle` never once reported a stall (`dmactrue=0`), while
`dohdma=225/frame` counts the call itself — an eight-channel scan that finds
nothing enabled every scanline. Kart's Mode 7 perspective is written per line
from an H-timer IRQ, not by an HDMA channel. The 226 calls were real; the work
behind them was not.

Where HDMA does run it is still small. Suzuka 8 Hours' rig window stalls 1,374
two-dot steps a frame — 2,748 dots of 357,368, or 0.77% of the dot clock.

### The fold that follows from that, and why it is not shipped

`run_dots`' DMA branch steps an HDMA stall **two dots at a time**, calling
`dma_cycle` for each pair, when the stall is a countdown whose end state is
arithmetic. Folding it is exact — `hdmaTimer` only ever gains multiples of 8 and
16, so it is even, and the loop body is linear in `dots`. Built in both copies
of the loop (`main_snes.c` and `tools/m7_qemu_rig/rig_snes.c` — they are one
program and the hashes police it) and measured on Suzuka, 1200 rig frames:

| arm | insn/frame | |
|---|---|---|
| baseline, inline two-dot step | 4,536,365 | |
| fold, in a `noinline` helper | 4,546,225 | **+9,860** |
| the `noinline` helper alone, no fold | 4,551,339 | **+14,974** |

`STATEHASH=804e6d24 AUDIOHASH=1b475063` on all three, so the fold is correct and
the helper changes nothing observable. It simply costs more than the stall is
worth: the codegen change to `run_dots` — the hottest loop in the core — is
larger than 1,374 iterations of a countdown. The first version, folded inline
without the helper, cost +12,315 on the same measurement.

Reverted. **Do not re-propose**: the prize is 0.3% on the one cartridge the
handoff named, and zero on Zelda and Kart, which activate no channels.

## Where Mario Kart's frame actually goes

Play scene, savestate resumed, 64-frame window after 600 warm-up frames,
`SNES_DEVICE_PROFILE=1`. 52.7 emulated fps, 13.1 drawn.

| block | drawn frame | skipped frame |
|---|---:|---:|
| PPU | 25.0% | 0.3% |
| APU (SPC + DSP) | 19.3% | 27.5% |
| `core_rem` (CPU + DMA + scheduler) | 48.1% | 72.0% |
|   — 65816 interpreter (`cpu_only`) | 27.9% (21.4% probe-corrected) | 41.3% |
|   — general DMA | 1.0% | 2.3% |
|   — HDMA | 0.3% | 0.5% |
|   — **event scheduler, the residue** | **18.9%** | **27.9%** |
| present/audio outside the emulation | 18.3% | 13.1% |

12,806 opcodes a frame. The scheduler residue is 1,619,020 cycles, i.e. **~126
cycles of frame-loop machinery per opcode against ~144 cycles of interpreter**
(probe-corrected). Nearly half of the CPU-side frame is not the CPU.

That residue is `run_frame_events` + `snes_handle_pos_stuff` (1,048 calls a
frame) + `dots_to_next_event` + the body of `run_dots`' loop — the DMA test, the
`cpuCyclesLeft == 0` test, `apply_irq_match`, the chunked consume, and the
`hPos`/`apuDotsAccum` updates. It is ordinary C in `main_snes.c`, and it is the
largest block in this core that has never been attacked.

## Two instruments were measuring a different program

Both were found by this session and both are fixed; every number above was taken
after the fix.

**The rig had no DSP-1.** `tools/m7_qemu_rig/run_snes_t2.sh` did not compile
`dsp1_hle.c`, so `dsp1_alloc()` resolved to the weak stub returning NULL and
every DSP cartridge ran there with no coprocessor attached. A/B arms of the DSP
change came back byte-for-byte equal because the code under test could not be
reached — `rdrom=30980` in both. Any earlier rig measurement of a DSP cart is
void. Added to the source list.

**The device profiler outlined the per-opcode wrapper into the overlay.**
`app_main_snes` carries a section attribute putting the frame loop in ITCM
beside the engine. With `SNES_DEVICE_PROFILE=1` the DWT marks make `run_one_opcode`
too big to inline, gcc emits it as `run_one_opcode.isra.0` into plain `.text`,
and the linker sweeps that into `.overlay_snes` at `0x2405f29c`. A PC sample of
the profiler arm charged it **12.1% of the frame**, plus 1.0% for
`__run_one_opcode.isra.0_veneer` and 1.0% for `__gsnes__snes_thumb2_run_veneer`
— about 14% of a program the device never ships, and it inflates Ledger B's
scheduler residue by the same amount. The shipping build inlines it and has none
of those symbols. `run_one_opcode` now names the ITCM section itself, so the
profiler arm compiles the same shape (verified: `0x00000298`).

## A correction to the handoff's DSP target

The handoff names "the four-tap Gaussian interpolation, +1.2 drawn, which is an
`SMLAD` shape on this core" as the one DSP item with a number and no
implementation.

**The device does not run the Gaussian.** `dsp_getSample` takes the
`#if defined(SNES_LINEAR_INTERP) || defined(GNW_SNES_CORE)` branch — a two-point
lerp, one multiply — and `GNW_SNES_CORE` is on every SNES object
(`Makefile.common:2241`). The four `gaussValues[]` lookups and MACs are the
*host/reference* path. So `SNES_ABLATE_DSP_INTERP`'s +1.2 drawn fps was
measured against the lerp, and there is no `SMLAD` to write: the shape the
handoff proposes already costs one multiply.

(The ablation number is contaminated for the usual reason as well — deleting the
interpolation silences voices and the work downstream of them disappears too.)
