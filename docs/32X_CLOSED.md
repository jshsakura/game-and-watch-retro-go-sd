# Sega 32X (picodrive) — closed for performance work

> **2026-08-14: the verdict below stands on speed and is wrong about the
> screen.** Everything in this file is *emulated* fps measured with the
> profiler on, in a boot-anchored scene. Re-measured with the instruments the
> SNES campaign produced — shipping build, real scene, and a counter for the
> frames the player actually sees — the core was drawing **one frame in four**,
> because it inherited a shared constant sized for a core where drawing is
> expensive. Doom went from **4.98 to 18.33 drawn fps** with no emulation change
> at all, and the arithmetic below did not move by one cycle. Read
> [§0](#0-what-2026-08-14-changed) before quoting any number in this file.

**Verdict, 2026-07-27: this core does not reach a playable frame rate, and no
remaining emulator-side change gets it there. Do not reopen the performance
axis without reading the ledger below.**

## 0. What 2026-08-14 changed

Nothing about the speed of emulation. Everything about what reaches the LCD.

> **Re-measured 2026-08-14 on the new counter — the result holds.**
> `g_common_drawn_frames` has since moved from the overload guard's decision to
> `lcd_swap()`, the one place that flips the panel. Both arms re-run back to
> back, each with its own `cores/32x.bin` on the card (verified by md5 against
> the arm), Doom, two samples each:
>
> | arm | emulated fps | drawn fps | ratio |
> |---|---:|---:|---:|
> | `MD32X_FORCED_DRAW_RATIO=4` (the old shared default) | 30.77 / 32.02 | 7.69 / 8.02 | 0.2500 |
> | per-core 1 (what ships) | 28.77 / 28.79 | **28.77 / 28.79** | 1.0000 |
>
> **×3.67 the visible frames for 8.3% of emulated fps** — the same trade the
> first measurement found (×3.7 for 8%).
>
> ⚠️ **The absolute numbers are higher than the first pass and I did not
> establish why.** Both arms moved together (4.98 → 7.85 at ratio 4), so the
> ratio and the relative cost are unaffected, but the baseline is not the same
> machine it was that morning. Candidates not separated: what the testbed merge
> brought in (the resident image shrank 261,228 → 255,324 B), and host load
> during the earlier runs. **Treat the absolute fps in the tables below as
> indicative and the ratio/relative cost as the result.**

**The honest baseline.** No `MD32X_DEVICE_PROFILE` (it costs ~16 of every 94
cycles an instruction takes — §14), cold boot into the attract demo rather than
a title screen, two samples per arm, and both counters:

| cartridge | emulated fps | **drawn fps** | ratio |
|---|---:|---:|---:|
| Doom | 20.13 / 19.73 | **5.02 / 4.93** | 0.249 |
| Knuckles' Chaotix | 15.42 | **3.87** | 0.251 |
| Kolibri | 9.89 | **2.48** | 0.250 |

Two things fall out. Emulated fps is **higher** than this file reports (20 vs
"10 → 13-16"), because the tax is gone. And the ratio is 0.25 on every
cartridge — pinned exactly on the overload guard's forced-draw floor.

**The floor was the draw rate, and it was another core's constant.** Sweeping it
on Doom:

| | emulated | drawn |
|---|---:|---:|
| 1-in-4 (what shipped) | 19.9 | 4.98 |
| 1-in-2 | 19.0 | 9.52 |
| **1-in-1** | 18.3 | **18.33** |

Drawing every frame costs **8% of emulated fps** and returns **3.7x the visible
frames** — and §"Why 2x is arithmetically out of reach" already says why it is
nearly free here: **draw is 1.9% of a 32X frame**. Skipping it saves 1.9% of the
work and throws away 75% of the player's frames. There is no real-time deadline
to protect either, since the core runs at about a third of console speed
whatever it does. On SNES the same constant is right (a drawn frame is 17.65 ms
against 14.6 ms of emulation; 1-in-3 underruns the audio), which is where 4 came
from. It is now a per-core choice — `common_emu_set_forced_draw_ratio()`, with
md32x asking for 1.

Gains across the library, drawn fps: Doom ×3.7, Chaotix ×3.4, Kolibri ×3.8.

**And §13's last open question is answered, in the other direction.** "2D titles
were never measured; that decides whether the core is unusable or only 3D is."
They are measured now, and **the 2D titles are slower than Doom** — Chaotix 3.87
and Kolibri 2.48 against Doom 4.98 drawn (13.12 / 9.40 / 18.33 with the ratio
fixed). The QEMU rig agrees: Chaotix costs 21.3 M host instructions a frame
against Doom's 7.9 M in a comparable window. Chaotix drives the SH-2s hard
despite being a 2D game. **Do not reopen the core on the hope that 2D is light.**

**Is it a 32X property? Not established, and the first attempt to answer it was
wrong.** Virtual Boy was measured at 35.90 emulated fps against 9.00 drawn
(ratio 0.2506) and written up here as a second core pinned on the floor. That
was retracted the same day: `main_vb.c` **discards the guard's answer and
presents every frame**, so the 9.00 counted the guard's decision, not anything
on VB's screen. VB draws all 35.9. `g_common_drawn_frames` means "frames the
guard asked for", which is the same thing as "frames presented" only for a core
that obeys the guard — 32X does, which is why the numbers above hold, and any
other core has to be read against its own loop first.

**And the "1-in-1 does not boot VB" claim that stood here is withdrawn too.** It
boots: 36.80 / 36.20 emulated fps at ratio 4 against 36.80 / 36.50 at ratio 1,
once each arm carries its own `cores/vb.bin`. The failure was a **stale core
file** — one arm's `vb.bin` under another arm's internal flash — which is the
same class of mistake as measuring through the wrong ELF, and it happened twice
in one afternoon. The 32X numbers above are not exposed to it because
`arm32x.sh` pushes `32x.bin` and `32x.xip` on every flash; `arm.sh` now takes
`PUSH_CORE=<core>` for the same reason.

So the per-core opt-in is justified by measurement discipline rather than by a
crash: nothing is known to break at ratio 1, and nothing is known to gain from
it either except a core that actually obeys the guard.

## 0b. D32XR — reachable, and the fault is ours

Doom 32X Resurrection replaces the retail engine entirely: rendering spread
across BOTH SH-2s, game logic at 15 fps against input at 30, resolutions down to
80x90. It is the only remaining lever that changes the *amount of work* rather
than the speed of the interpreter, which is why it was worth a day.

**It does not run on this core, and it is not picodrive's fault.** Upstream
vanilla picodrive (26ecb2b6, libretro headless) runs the same ROM: 432 of 600
frames live, first render at frame 168. Our fork wedges — the SH-2s run for a
while at 167,287 instructions a frame and then stop dead, and the framebuffer
stays black throughout.

**Closed by experiment, in order** — do not re-derive any of these:

| suspect | verdict |
|---|---|
| the SSF (>4 MiB bank) mapper we compile out | innocent — a 4 MiB build wedges identically |
| the wad trimmed to one level to reach 4 MiB | innocent — the unmodified 5 MiB release wedges identically |
| unmapped ROM-mirror probe reads at f181 | innocent — three arms (absent / mirrored / mirrored-with-a-pattern) fail identically, so the values are discarded: wild reads, and the cause is earlier |
| `0xA15106` semantics, low-ROM write-drop | identical to upstream |
| the SH-2 fastloop lever | innocent — off changes nothing |
| **the 68K core** (our fork swaps picodrive's FAME for gwenesis' g68k) | innocent — FAME swapped back, identical wedge, and SH-2 per-frame counts are byte-identical across both |
| **the real 32X BIOS** | innocent as a *requirement*: upstream runs the ROM with picodrive's synthesised stub. In OUR fork the stub wedges at frame ~60 and the real BIOS pushes it to ~180, so the BIOS masks the defect rather than supplying anything the game needs. **A first reading of that as "D32XR requires the BIOS" was wrong.** |

**Where it points now:** the SH-2 side, and specifically the things this fork put
there — the inlined instruction fetch (`GNW_FETCH_SD`, `sh2pico.c`, whose own
comment records that Doom's master SH-2 fetches 100% from cart ROM), the
opcode-pattern idle folds, `p_rom` pointing at a byteswapped zero-copy image
where upstream has a RAM copy, and the 32X memory map's GNW guards. A fetch that
returns one wrong halfword explains the symptom exactly: runs correctly for
thousands of frames, then executes something that is not the program.

The rig can supply the real BIOS (`RIG_32X_BIOS=<dir>`) and bank a >4 MiB cart
(`RIG_32X_SSF=1`); both exist only for this investigation.

**What this does not change.** The console still runs at roughly a third of
speed, so games play in slow motion with slow audio; smooth is not the same as
correct. Every cycle-count and closed axis below is untouched.

Measured on hardware (STM32H7B0 @ 312 MHz, `MD32X_DEVICE_PROFILE=1`):

| Game | fps |
|------|-----|
| Doom | 10 → 13-16 over one day of optimisation (≈15 without the profiler) |
| After Burner Complete | 8 (≈9.5 without the profiler) |

60 fps needs a frame to fit in 5,197,920 device cycles. It costs ~24,000,000.
That is **4.6x**, and the arithmetic below says where the 4.6x cannot come from.

The core stays in the launcher — it runs, it is accurate, and some titles may
be usable — but it is not a performance target any more.

## Why 2x is arithmetically out of reach

One heavy-scene frame, 24.4 M device cycles:

| phase | cycles/frame | share |
|-------|-------------|-------|
| **msh2** | **16.3 M** | **67.9%** |
| m68k | 3.1 M | 12.9% |
| ssh2 | 2.1 M | 8.6% |
| FM (YM2612) | 0.56 M | 2.3% |
| draw (MD + 32X compositor) | 0.48 M | 1.9% |
| rest | ~1.8 M | ~6% |

Zeroing **everything except msh2** removes 7.3 M — less than half of what 2x
needs (12.2 M). So 2x requires cutting the master SH-2 by 60-70%.

msh2 is 210 k dispatched guest instructions per frame at ~92 device cycles
each. Cutting the per-instruction cost to ~45 (an optimistic floor for a
hand-tuned C interpreter) is msh2 −47% → frame −34% → **fps ×1.5**, and that is
the ceiling of that approach. Below 45 you have to stop interpreting, which
means a dynarec — and there is no room for one (see the ledger).

## Ledger of closed axes — all closed by measurement, not by opinion

Two of these were built, measured and reverted. None was rejected on a hunch.

| Axis | Result | Where |
|------|--------|-------|
| Write path (every SH-2 store is an indirect call) | Read handlers + write tabs together are **1.1%** of msh2's wall, 15-20 cyc/call | ledger probe, since deleted; numbers kept in `pico/32x/memory.c` |
| Inlining guest data loads | Checksums identical, **host instructions +3.6%** — rejected on the rig, never shipped | `tools/m7_qemu_rig/run_32x.sh` |
| External-flash (XIP) latency | Opcode fetch is **10.3-10.4 cycles**, ~11% of the wall, and *identical across two very different scenes*. The D-cache absorbs it, so caching ROM in RAM was never the answer | fetch probe, `cpu/sh2/mame/sh2pico.c` |
| **A dynarec** | RAM_EMU is 692.5 KB of which **656 KB is the emulated console's own memory** (32X SDRAM 256 K + DRAM framebuffers 2×128 K + Genesis 68K RAM/VRAM 136 K). A translation cache needs 256-512 KB. **There is nowhere to put it** | enumerated from `build/gw_retro_go.map` |
| Code placement (ITCM) | Already spent — the whole interpreter TU has been in ITCM since the +30% move | `STM32H7B0VBTx_SDCARD.ld`, `.overlay_md32x_itc` |
| State placement (DTCM) | SH-2 register file moved to DTCM: **94.0 → 92.1** cycles/insn, inside scene noise; per-event costs flat. Reverted — it also charged 12 KB of the shared DTCM heap | picodrive `1c78adf8` → `d4317e53` |
| QEMU access census | QEMU models no cache, so it can count accesses but never price them — which was the question | `RIG_MEM_MIX` in `rig_32x.c`, left in place, documented as impractical |

### The cost model that closes it

Per dispatched msh2 instruction (94.0 cycles, profiler build):

```
opcode fetch      10.3 x 1.00 = 10.3
guest loads       29.3 x 0.37 = 10.8
guest stores      38.5 x 0.06 =  2.3
                  ------------------
memory total                    23.4   (25%)
decode + execute               ~70     (75%)
```

Memory is a quarter. The interpreter's own decode/execute is the rest, and its
code already runs from zero-wait ITCM. That is why both placement axes came up
empty: there was nothing left to place.

## What shipped and stayed

- **Master frame-wait spin fold** — the 128 B page holding it went from 35.0% of
  the master's wall to 0.2%; frame wall −18.8% on the same scene. Cycle-exact,
  opcode-fingerprinted (never a ROM hash), gated by 402 bit-identical frames.
- **Cart-ROM opcode fetch inlined** — the fast path only ever covered SDRAM,
  while the device shows msh2 running 100% out of cart ROM. −17.7% host
  instructions on the rig.
- **Guest-bus entry points into ITCM** — the interpreter is in ITCM and
  `p32x_sh2_read/write8/16/32` were in RAM_EMU, 603 MB away, so every guest
  load and store paid a long-branch veneer. Now direct; the rare direction
  (on-chip DMA, IRQ vectors, XIP-resident callers) pays instead.

## Still open, and deliberately left

- **68K, 12.9% of the frame.** A probe is in the source (`MD32X_DEVICE_PROFILE`,
  `cpu/gwenesis68k/m68kcpu.c`) that reports guest cycles run vs poll-skipped.
  picodrive already skips a 68K that polls a *32X register*
  (`m68k_poll_detect` → `SekSetStop`); whether Doom's wait is that kind of poll
  was never measured. If `skip_pct` is near zero, the spin-fold idiom applies
  again. Worth at most ~7% — it was not worth another device round trip once
  the verdict was in.
- **D32XR (Doom 32X Resurrection).** A rewritten Doom that does far less work.
  Zero code on our side; swap the ROM and compare the same scene. Never tested.
- **2D 32X titles.** Everything measured (Doom, After Burner) is the heaviest
  class — SH-2 software 3D and sprite scaling. Knuckles' Chaotix, Mortal
  Kombat II, NBA Jam TE use the 32X mostly for colours and sprites and were
  never measured. If someone reopens this, measure one of those first: it is
  the difference between "the core is unusable" and "3D titles are unusable".
- **Fullscreen.** There is no scaler. The ten cores that offer one render into
  their own framebuffer and blit through a nearest-neighbour scaler; 32X (like
  MD) has picodrive paint straight into the LCD active buffer
  (`PicoDrawSetOutBuf(lcd_get_active_buffer(), 320*2)`), deliberately, because a
  320×240×2 intermediate is 150 KB and the overlay had 328 B. V28 (320×224) is
  centred at rows 8..231, hence 8 px bars. Implementable with **no extra RAM**
  as an in-place vertical expansion of the finished LCD buffer (224→240,
  duplicate one row in 14; top half top-down, bottom half bottom-up), ~150 KB of
  row moves per drawn frame — under 1% at current frame times. Must be matched
  with `md32x_border_clear_set_content_rect`.

## If you measure this core again, read this first

- **Device dumps cannot be A/B'd across scenes.** `cycles/guest-insn` moved
  85.7 ↔ 101.6 with the code unchanged, because a busier scene has better
  locality and longer interpreter slices. The only scene-stable numbers are the
  **per-event** ones (fetch 10.3, load 29.3, store 38.5) — those repeated to
  within 0.1 cycles across wildly different scenes. Use those, or use the rig,
  which replays a fixed input script.
- **The profiler is not free.** `MD32X_DEVICE_PROFILE=1` costs ~16 of the 94
  cycles per instruction (~17%). Every fps number above was measured with it on
  unless stated otherwise. It is out of release builds again.
- **The dump window opens after 1200 warmup frames (~20 s).** Boot-anchored
  windows measured the title screen three times before this was fixed, and the
  page they blamed (51.4% of the wall) turned out to be 0.7% during gameplay.
  Play for 20 s or the file will not appear at all.
- **Linker placement must be verified in the map, never by "it built".** Twice
  in one day a placement rule matched nothing — `.overlay_md32x*` is matched
  first and its `(.text .text*)` / `(.bss .bss*)` globs had already claimed the
  sections — and the link succeeded, the firmware booted, and nothing moved.
  GNU ld cannot exclude an input section by name; give the code its own section
  name in the source instead (`GNW_SH2BUS` in `pico/32x/memory.c` is the
  pattern), and make the rig's script claim it too or it becomes an orphan.
- **`make docker` silently drops command-line variables.** It hardcodes its
  inner make line. Use `make release DOCKER=1 <VARS>`, which forwards
  `MAKEOVERRIDES`. A margin figure measured the wrong way is worse than none.

## Where the detail lives

- **`docs/32X_DEVICE_MEASUREMENT_LOG.md`** — every experiment of the device
  campaign in order, failures included, each with hypothesis, instrument,
  numbers and verdict. Its first section answers the question this core will
  keep attracting: **why there was no GBA-style trick here.** Read that before
  proposing an HLE.
- `docs/32X_PERFORMANCE_RESULTS.md` — the earlier rig-driven optimisation.
- `docs/32X_PERFORMANCE_HISTOGRAM_GUIDE.md`, `docs/32X_RIG_ANALYSIS.md` — how
  the QEMU rig works and how to read its histograms.
