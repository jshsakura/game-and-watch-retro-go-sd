# SNES 4bpp tile decode cache: M7 rig result

Date: 2026-07-20

## Decision

Do not enable a decoded 4bpp tile cache in the generic SNES PPU.  On the M7
instruction rig every correct SMW variant was slower than the existing direct
bitplane renderer.  The smallest cache also diverged on Zelda.  All experimental
PPU changes were removed after the measurements below.

This is a useful negative result: GCC's existing `PpuDrawBackground_4bpp` output
keeps the two VRAM words in registers and extracts pixels with cheap shifts.  A
cache removes those shifts but adds a tag/validity lookup for every tile row and
extra work on VRAM writes or cache misses.  The lookup cost is larger than the
work removed.

## Gate and baseline

Command shape:

```sh
tools/m7_qemu_rig/run_snes_hf.sh <rom> 300
```

The cache candidates used `RIG_EXTRA_DEF=-DSNES_PPU_4BPP_CACHE`.  The rig builds
the real `external/sm/src/snes/ppu.c` for Cortex-M7 hard-float and reports ARM
executed instructions plus the normal state/audio hashes.

| ROM | Baseline emu insn/frame | STATEHASH | AUDIOHASH |
|---|---:|---|---|
| SMW (`external/smw/smw.sfc`) | 5,203,376 | `a68c36b5` | `a5589af4` |
| Zelda ALttP (`roms/zelda_alttp.smc`) | 4,407,809 | `6efe6e66` | `a181f157` |

## Variants rejected

All SMW numbers below used the same 300-frame window and preserved both hashes.

| Cache organization | BSS cost | SMW emu insn/frame | Delta |
|---|---:|---:|---:|
| Packed rows + validity bitmap | about 66 KB | 5,235,280 | +0.61% |
| Packed rows + byte validity | about 80 KB | 5,232,088 | +0.55% |
| Fully decoded byte rows + byte validity | about 144 KB | 5,229,952 | +0.51% |
| Eager decode on VRAM writes | about 144 KB | 5,240,299 | +0.71% |
| Dirty-row queue, flush before drawing | about 176 KB | 5,243,880 | +0.78% |
| 1024-entry direct-mapped packed cache | 8 KB | 5,232,527 | +0.56% |
| 1024-entry direct-mapped byte cache | 10 KB | 5,231,255 | +0.54% |

The final 10 KB direct-mapped candidate produced 4,404,541 emu insn/frame on
Zelda (-0.07%), but its `STATEHASH` changed from `6efe6e66` to `70202c34`.
That candidate therefore failed correctness as well as failing the cross-ROM
performance requirement.

## Boundary for future work

Do not retry a generic per-row decode cache unless the renderer changes enough
to eliminate the per-tile lookup entirely.  A promising PPU optimization must
remove a larger unit of work (for example a proven-safe complete layer/scanline
reuse or a game-specific semantic fast path), not trade register shifts for
more RAM traffic.
