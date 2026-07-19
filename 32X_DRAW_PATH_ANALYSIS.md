# 32X Draw Path Analysis — DMA2D Feasibility + Cache Pattern (0720)

Scope: the `draw+32x` DWT bucket (26.7% of frame time on the last device
measurement, msh2 63.3% / ssh2≈0 / draw+32x 26.7%). Two pprof buckets make up
this figure — confirmed from source, not assumed:

- `pp_draw` — `PicoDrawSync()` in `external/picodrive/pico/draw.c:1871`, the MD
  (Genesis) VDP line renderer: `BackFill` → `DrawLayer`(BG/FG tiles) →
  `DrawWindow` → `DrawAllSprites`/`DrawSpritesSHi` → `FinalizeLine`.
- `pp_draw32x` — `Pico32xRenderSync()` in `pico/32x/32x.c:271`
  (`#define pp_draw pp_draw32x` at `32x.c:17` redirects the generic
  `pprof_start(draw)` calls in that TU into this bucket), which calls
  `PicoDraw32xLayer()` in `pico/32x/draw.c:283` — the 32X/MD layer compositor.

No rig/QEMU instruction counts are used anywhere below to argue a magnitude —
per project rule, `icount` cannot see memory-bandwidth/stall cost, which is
exactly what this bucket is made of. Everything here is source-level algorithm
analysis plus register-level HAL/CMSIS facts. Magnitude claims need device DWT
(`MD32X_DEVICE_PROFILE=1`), not this document.

## ① Is the 32X→LCD path currently a CPU pixel loop?

Yes, and there is no separate "framebuffer → LCD" blit stage to even target —
picodrive renders **directly into the LCD's active buffer**:

- `set_out_buffer()` (`main_md32x.c:194`): `PicoDrawSetOutBuf(lcd_get_active_buffer(), 320*2)`.
- `PicoDrawSync()`'s `FinalizeLine` and `Pico32xRenderSync()`'s
  `PicoDraw32xLayer()` both write straight into that pointer
  (`est->DrawLineDest` / `Pico.est.DrawLineDest`).

So "compositing" and "the CPU pixel loop" are the same code — there is nothing
downstream of it to hardware-accelerate; the loop itself is the candidate.

## ② DMA2D feasibility — three sub-paths, three different answers

Register-level facts used below, all read from the vendored HAL/CMSIS for
this exact chip (not general STM32 knowledge, since DMA2D varies by family):

- `Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_dma2d.h`: transfer modes are
  `DMA2D_M2M`, `DMA2D_M2M_PFC`, `DMA2D_M2M_BLEND`, `DMA2D_R2M`,
  `DMA2D_M2M_BLEND_FG`, `DMA2D_M2M_BLEND_BG` (line 209-214). Blending is a
  fixed `OUT = FG·α + BG·(1-α)` equation using either a per-pixel alpha
  channel (from an ARGB* input format) or a constant `AlphaMode`/`AlphaValue`
  override (`DMA2D_OutputCfgTypeDef`/`DMA2D_LayerCfgTypeDef`, lines 64-118).
  There is no conditional/compare capability anywhere in the peripheral.
- **No color-keying hardware on this chip.** Some STM32 families' DMA2D has a
  `CKCFGR` chroma-key register (F4/F7); grepping both the HAL driver header
  and the CMSIS device header
  (`Drivers/CMSIS/Device/ST/STM32H7xx/Include/stm32h7b0xx.h`) for
  `CKCFGR`/`ColorKey`/`FGCKCR`/`BGCKCR` returns nothing on this part. Even if
  it existed, color-keying only tests the pixel DMA2D is currently
  transferring, not a *different* surface's raw value — see below.
- Input color modes (line 234-245) include `DMA2D_INPUT_L8` (8bpp
  indexed/CLUT); CLUT loading is a first-class feature
  (`DMA2D_CLUTCfgTypeDef`, `HAL_DMA2D_ConfigCLUT`/`CLUTLoad`, lines 48-59,
  513-538).

### `PicoDraw32xLayer` (32X compositor) — not feasible

Every one of the three 32X video modes (`do_line_dc`/`do_line_pp`/`do_line_rl`
in `pico/32x/draw.c:63-121`) selects MD-layer-vs-32X-layer **per pixel** by
comparing the MD layer's *raw palette index* (`*pmd`, before it becomes a
color) against a runtime register (`mdbg = Pico.video.reg[7] & 0x3f`, the MD
backdrop color), and separately gates the 32X layer by a priority/alpha bit
(`t & 0x8000` in Direct Color mode, `t & PXPRIO` in Packed Pixel/Run Length).
Both conditions can flip mid-scanline and depend on live VDP register state.

DMA2D's blend equation has no register-compare primitive, and the comparison
operand (the MD layer's *raw index*, not its resolved color) doesn't even
exist in a DMA2D-visible buffer — by the time a color is written anywhere,
the index information DMA2D would need is already gone. Direct Color mode's
`t & 0x8000` alpha-bit gate superficially resembles ARGB1555 alpha blending,
but `do_line_dc` OR's that gate with the *separate* backdrop-index test
(`(*pmd & 0x3f) == mdbg` forces the 32X pixel through regardless of the alpha
bit) — a compound condition no single DMA2D blend pass expresses.

Even setting the algorithm mismatch aside: this runs up to 320 times/frame
(`PicoScan32xBegin/End`, once per scanline, because of the 32X line-shift
register `P32XV_SFT` and per-line DRAM bank pointer) at 320×1 px = 640 bytes
per transfer. DMA2D configuration is ~8-10 register writes
(FG/BG `MAR`/`OR`/`PFCCR`, output `MAR`/`OR`/`OPFCCR`, `NLR`, `CR`) plus a
start-and-poll/IRQ-wait per transfer — fixed overhead a transfer this small is
unlikely to amortize, independent of the algorithm question.

### `pp_draw`'s tile/sprite stages (`DrawLayer`/`DrawWindow`/`DrawAllSprites`) — not feasible

These decode the VDP nametable and pattern table (`pack = CPU_LE2(*(u32 *)(PicoMem.vram + addr))`
in `DrawTile`, `draw.c:306-335`) and resolve sprite priority/shadow-highlight
per pixel (`pix_sh`/`pix_as`/`pix_sh_as` macros, `draw.c:206-301`). DMA2D has
no tile/pattern-table decode primitive at all — it moves and format-converts
pixels from a *linear* source, it cannot dereference a nametable to find
where to read from. Not applicable, full stop.

### `FinalizeLine555`'s MD CLUT pass — the one algorithmically clean candidate

`FinalizeLine555` (`draw.c:1505`) does, in its plain (no softscale, 320-wide)
path: `dst[x] = HighPal[src[x]]` for 320 pixels — a pure 8bpp-indexed → RGB565
lookup, no branch. `HighPal` is a genuine 256-entry LUT
(`unsigned short HighPal[0x100]`, `pico_int.h:456`) rebuilt only when the
palette is dirty (`PicoDrawUpdateHighPal`), not per-pixel. This is *exactly*
`DMA2D_INPUT_L8` → CLUT → `DMA2D_OUTPUT_RGB565`, `DMA2D_M2M_PFC` mode — no
register/format mismatch at all.

Two things matter for whether this is worth doing:

1. **It genuinely runs, unconditionally, every scanline** — confirmed via
   `Pico32xDrawMode`. `PicoDrawSetOutFormat32x` (`pico/32x/draw.c:391`) sets
   `Pico32xDrawMode = (which == PDF_RGB555) ? PDM32X_32X_ONLY : PDM32X_BOTH`,
   and `emu_32x_startup()` (`main_md32x.c:207`) always calls
   `PicoDrawSetOutFormat(PDF_RGB555, 0)` — so GNW's 32X core always runs in
   `PDM32X_32X_ONLY`, never `PDM32X_BOTH`. That means `PicoDraw32xLayer`'s
   dispatch never selects the `_md`-suffixed `do_loop` variants (see
   `draw.c:325-330`), i.e. it **never** re-derives `palmd[*pmd]` itself for
   MD-layer pixels — it only conditionally overwrites what's already in the
   destination. That "already there" value is written earlier, per scanline,
   by `PicoDrawSync → PicoLine → FinalizeLine (=FinalizeLine555)`. So the
   two passes are genuinely separate at runtime, and the CLUT pass is not
   redundant work being hidden inside the compositor.
2. **Same per-line-dispatch overhead question as above.** A naive
   "blocking DMA2D call per scanline" replacing a ~320-iteration lookup loop
   is not obviously a win — DMA2D's setup/poll tax on a 640-byte transfer may
   cost more than the loop it replaces. The one thing that *would* make it a
   real win is exploiting that DMA2D is a **separate bus master**: kick the
   DMA2D transfer for line N's CLUT conversion in non-blocking (IT) mode, and
   let the CPU immediately start decoding line N+1's tiles/sprites into a
   second `HighCol` buffer while DMA2D runs concurrently — only synchronizing
   before `PicoDraw32xLayer` needs line N's converted pixels. That turns the
   DMA2D cost from serial overhead into (mostly) hidden latency. This needs:
   double-buffering `HighCol` (2×328B, trivial), moving the `HighPal`→CLUT
   load to happen once per dirty-palette event (not per line), and a small
   restructuring of `PicoDrawSync`'s loop to pipeline by one line. This is a
   real structural change to the render loop, not a drop-in swap.

**Verdict: feasible in isolation, uncertain in practice.** I did not
implement this. Implementing a pipelined double-buffered version blind, then
claiming a win from QEMU icount, is exactly the mistake the project's rules
warn against (rc/idle-skip both "died" this way). It needs device DWT
before/after on the *same* `pp_draw` bucket to know if it's worth the
complexity — and even a full win here likely only shaves a fraction of
`pp_draw`, since `DrawLayer`/`DrawWindow`/`DrawAllSprites` (not
DMA2D-eligible, see above) still dominate that bucket's tile/sprite work.

## ③ Cache-friendly access pattern (VB blit precedent)

No VB-style bug found. VB's win was fixing a *genuine implementation mistake*
(writing output pixels in column-major order into a row-major framebuffer, an
artifact of VB's actual rotated-panel hardware). Checked the equivalent
hot-loop memory patterns here:

- `do_line_dc`/`do_line_pp`/`do_line_rl` (32X compositor): `pd++`, `pmd++`,
  `p32x++` are all incremented together, strictly sequential — no stride.
- `Pico.est.Draw2FB` access in the `do_loop_*` macros: 328-byte-wide rows
  (320 px + 8 px border), accessed sequentially within a row
  (`pmd` walks the row), row-to-row via `pmd += 8` in the outer per-line
  loop — row-major, not strided.
- `DrawTile`'s VRAM fetch (`draw.c:306-335`): non-sequential *by design* —
  it follows the nametable's tile codes, which can point anywhere in VRAM.
  This is not a stride mistake to fix; it's what tile-based rendering *is*.
  picodrive already reads it as one packed 32-bit fetch per tile-row
  (`CPU_LE2(*(u32*)...)`), which is the standard mitigation for this pattern
  (4 pixels per VRAM access instead of 4 separate byte reads).

Conclusion: this renderer (notaz's picodrive C path, already tuned for
GP2X/PSP-class ARM9/ARM11 cores) doesn't have an equivalent low-hanging
fruit. I'm reporting the negative result rather than manufacturing a change.

## Bonus finding: upstream hand-tuned ARM assembly exists but doesn't apply here

`external/picodrive/pico/draw_arm.S`, `draw2_arm.S`, `32x/draw_arm.S` are
hand-written assembly versions of exactly these hot loops (guarded by
`_ASM_DRAW_C`/`_ASM_32X_DRAW`, currently unused in the GNW build). They are
**not portable to Cortex-M7 as-is**: they use classic ARM (A32)
application-profile idioms — `stmfd`/`ldmfd` full-descending-stack mnemonics
and the `mov lr, pc` / `ldr pc, [...]` indirect-call trick
(`32x/draw_arm.S:43-50`) — that rely on A32 PC-relative-fetch-ahead semantics.
Cortex-M7 is Thumb-2-only (M-profile, no A32 execution state at all); this
call idiom doesn't have a Thumb-2 equivalent and the file would need a
substantial rewrite, not a recompile. Noted as a possible future lever, not
an actionable one now — the port effort is comparable to writing new
Thumb-2 assembly from scratch, with the same correctness risk, and is
out of scope for this pass.

## Summary

| Sub-path | DMA2D feasible? | Why |
|---|---|---|
| 32X compositor (`PicoDraw32xLayer`) | No | Per-pixel selection compares MD's *raw index* (not color) against a runtime register + a priority bit — no DMA2D primitive for this; no color-key HW on this chip either |
| MD tile/sprite render (`DrawLayer`/`DrawWindow`/`DrawAllSprites`) | No | Nametable/pattern-table decode — DMA2D has no lookup/decode capability |
| MD CLUT pass (`FinalizeLine555`) | Algorithmically yes; practically unproven | Exact match for `DMA2D_INPUT_L8`+CLUT→`DMA2D_OUTPUT_RGB565`, `M2M_PFC` mode. Runs unconditionally every line (confirmed: GNW always uses `PDM32X_32X_ONLY`, so this pass is not redundant with the compositor). Win depends on hiding per-transfer DMA2D overhead via IT-mode pipelining across scanlines — a real render-loop restructuring, needs device DWT to justify before implementing |
| Cache/stride pattern | No bug found | All hot loops already sequential; VRAM tile fetch is nametable-driven by design, already using packed 32-bit reads |

Next: device DWT split of `pp_draw` vs `pp_draw32x` specifically (the current
26.7% figure is combined) would tell us whether `FinalizeLine555`'s pipelined
DMA2D idea is even worth prototyping — if `pp_draw32x` (not DMA2D-eligible at
all) dominates the 26.7%, the CLUT-pass lever caps out low regardless.
