# SNES PPU line-renderer investigation (SMW)

Date: 2026-07-20

## Scope and gate

- Core: `external/sm` at `4124a93`, generic interpreter, `PPU_RGB565` device path.
- ROM: `/tmp/smw_ppu_deep.smc`, SHA-256
  `0838e531fe22c077528febe14cb3ff7c492f1f5fa8de354192bdff7137c27f5b`
  (identical to `external/smw/smw.sfc`).
- Command: `RIG_WINDOW=200 bash tools/m7_qemu_rig/run_snes_hf.sh /tmp/smw_ppu_deep.smc 1200`.
- Correctness gate: both `STATEHASH` and `AUDIOHASH` must be bit-identical.
- These are QEMU Cortex-M7 executed-instruction counts, not device fps or cycle
  predictions.
- `external/sm` remains an uncommitted submodule working-tree experiment.

## Baseline and final result

| Build | emu insn/frame | STATEHASH | AUDIOHASH | linked text |
|---|---:|---|---|---:|
| Baseline | 7,611,043 | `fd31800f` | `f9d150be` | 621,956 B |
| Final PPU experiment | 6,762,755 | `fd31800f` | `f9d150be` | 621,956 B |
| Delta | **-848,288 (-11.15%)** | identical | identical | **0 B** |

The final result was repeated after concurrent unrelated working-tree changes
appeared and reproduced exactly. The color-math change was also isolated by
removing only its two functional lines: control `6,981,177`, candidate
`6,762,755`, with identical hashes in both runs.

## Measured path use before changing it

`run_snes_ppu_deep.py` over the same 1200 frames measured total render-on minus
render-off PPU work at **2,808,285 insn/frame**. Its path counters reported the
following average pixel counts per frame:

- color-math loop: 39,994
- math applied: 26,429
- existing fixed-color/table shortcut: 17,021
- real opaque subscreen blend: 9,408
- half-color: **0**

The deep build increments 64-bit counters in pixel loops, so its individual
stage instruction buckets are instrumentation-heavy; the render-on/off total
and the path counts are the useful numbers here.

The proposed RGB555 half-add / `__SHADD16` lever was therefore rejected before
implementation: SMW executes zero half-color pixels in this 1200-frame window.
The blend inputs are CGRAM RGB555 and only the final store is RGB565, so a future
half-add experiment on another scene must preserve RGB555 channel boundaries
before brightness conversion and RGB565 packing; a constant copied from a
packed-output RGB565 formula would not be valid automatically.

## Accepted changes and incremental A/B

| Step | emu insn/frame | Incremental delta | Reason |
|---|---:|---:|---|
| Baseline | 7,611,043 | - | - |
| Reuse decoded 4bpp `pixel` for transparency | 7,552,244 | -58,799 | Removes a second bitplane mask after the pixel was already assembled. |
| SWAR 4bpp full-tile decode | 7,028,885 | -523,359 | Transposes four bitplanes once per 8-pixel tile, then extracts one nibble per pixel. No tile cache or LUT. |
| SWAR 2bpp full-tile decode | 7,024,421 | -4,464 | Same operation for BG3. |
| Skip BG3-high z comparisons | 6,981,177 | -43,244 | Mode-1 BG3 high priority `0xf2` is above the maximum prior BG/OBJ priority `0xe4`. |
| Direct `mathFixed565` index | 6,762,755 | -218,422 | `main_z & 0xfff` already equals contiguous `[layer][CGRAM index]`; avoids rebuilding the address per shortcut pixel. |

The SWAR conversion is only used for full middle tiles. Left/right clipped
fragments retain the original shift loop. All accepted changes together have
zero net linked-text growth in this build.

## Existing behavior confirmed, not retried

- Completely disabled layers already return before tile work.
- Windowed layers already render only enabled spans.
- BG rendering is already organized around 8-pixel full tiles with separate
  clipped edges; it is not a per-pixel tile-fetch renderer.
- Mode selection is outside the tile pixel loop.
- No 4bpp tile cache, fold LUT, dirty rect, idle skip, dispatch, ROM/WRAM path,
  event-loop, or recompilation experiment was repeated.

## Rejected experiment

Adding a loop-invariant `check_z` condition to skip BG1 z reads on lines without
sprites reduced another 8,578 insn/frame, but GCC cloned enough of the large
renderer to grow linked text by 960 B. It was reverted because the whole PPU is
ITCM-resident and that trade is poor.

## Working-tree ownership

This investigation changed only `external/sm/src/snes/ppu.c` plus this report.
Concurrent changes in `external/sm/src/snes/snes.c`, `snes.h`,
`Core/Src/porting/snes/main_snes.c`, and `tools/snes_diag/instrument.py` are not
part of these results and were left untouched.

## Follow-up: persistent-framebuffer scanline reuse

The follow-up was measured from submodule `4c64b00` with the shipping renderer
flags `SNES_DSP_MONO` and `SNES_PPU_DIRECT_MATH`.  Its OFF baseline also includes
two independently gated sprite micro-optimizations still present in the
submodule working tree: one 4bpp decode per sprite tile and removal of a
redundant cached-candidate height test.  Together those measured
`-11,332 insn/frame`, bit-identical, for `+64 B` text.

The observation-only predictor first rendered every line and compared the exact
256-pixel RGB565 result with the preceding frame.  With exact renderer-register
state, value-based VRAM/CGRAM/OAM generations, referenced VRAM-page masks,
referenced CGRAM-entry masks, and old/new sprite-candidate OAM masks, it reported
`135,704` reusable predictions out of `268,576`, with **zero false positives**.
The final cache uses the same inputs, but conservatively groups VRAM into
256-word pages.

The first literal implementation was not acceptable despite correct hashes:

| Cache policy | Overall emu insn/frame | Frames 900-1199 | Result |
|---|---:|---:|---|
| OFF | 6,613,231 | 7,451,306 | baseline |
| Check all lines every frame | 6,431,761 | 8,333,336 | active-play regression |
| Check only lines 0-63 and 192-223 | 6,548,159 | 7,858,237 | active-play regression |

Both rejected variants were hash-identical.  Their problem was predictor and
dependency-capture cost on continually changing lines, not correctness.

The retained experiment cools a line down for 16 frames after its first miss;
it then renders once to learn fresh dependencies and tests reuse on the next
frame.  Stable lines continue to hit without cooldown.  The exact final A/B was:

| Build | Overall emu insn/frame | Frames 900-1199 | STATEHASH | AUDIOHASH | text |
|---|---:|---:|---|---|---:|
| Cache OFF | 6,613,231 | 7,451,306 | `fd31800f` | `cf7c29b2` | 622,020 B |
| Cache ON | 6,054,251 | 7,324,281 | `fd31800f` | `cf7c29b2` | 624,684 B |
| Delta | **-558,980 (-8.45%)** | **-127,025 (-1.70%)** | identical | identical | **+2,664 B** |

Actual skipped-line counts were `100,127 / 268,800` (**37.24%**) overall and
`6,566 / 67,200` (**9.77%**) in frames 900-1199.  All four 300-frame framebuffer
and audio hashes also matched the OFF run.  The linked BSS increase was 49,768 B;
no pixel cache is allocated, because both the rig framebuffer and device
`snes_frame` are persistent.

`ppu_lineCacheInvalidate()` is called by `ppu_reset`, after `ppu_saveload`, after
each explicit `snes_frame` clear, and whenever `common_emu_state.clear_frames`
is active.  Thus the first frame, save/load boundary, and UI screen-clear
boundary force complete redraws.  These numbers are QEMU M7 executed
instructions only; they are not a device-fps prediction.
