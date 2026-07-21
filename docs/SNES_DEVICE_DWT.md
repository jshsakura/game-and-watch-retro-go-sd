# SNES device frame profiler — 3 ledgers, and the questions each one can answer

`SNES_DEVICE_PROFILE=1` builds a diagnostic firmware that measures where a SNES
frame actually goes **on the device**, and dumps it once to `/snes_dwt.txt`
after 64 frames.

It exists because every SNES performance decision to date was made on
`tools/m7_qemu_rig` instruction counts, and **that rig does not link
`main_snes.c`**. It never saw `common_emu_frame_loop()`, `present_frame()`, the
DMA2D wait, `lcd_swap()` or the audio pacing. "instructions ÷ clock = fps" is
only true if the frame is compute-bound *and* present/pacing cost nothing, and
nothing had checked either half on hardware.

## Build and run

```
make release DOCKER=1 COVERFLOW=1 SHARED_HIBERNATE_SAVESTATE=1 \
     DISABLE_SPLASH_SCREEN=1 ENABLE_BOOT_OC=1 INTFLASH_BANK=2 CHEAT_CODES=1 \
     ZH_CN=1 ZH_TW=1 KO_KR=1 JA_JP=1 SNES_DEVICE_PROFILE=1
```

Flash, load a SNES ROM from a savestate, play 64 frames, then read
`/snes_dwt.txt` (and `/snes_diag.txt`, which gets a boot line with the AHB
pre-flight and the wall-clock calibration) off the card.

Cost: **112 bytes of internal flash** (measured — headroom 1,668 B → 1,556 B on
the canonical release flag set), 10,240 B of AHB, ~2 KB in the SNES overlay,
which has ~288 KB free. Nothing needed the `RCPROBE_CODE` sentinel.

`SNES_DEVICE_PROFILE=0` (the default) is byte-identical to a tree without this
feature: no Python runs, `external/sm` compiles untouched, and the two resident
IRQ hooks compile away.

## The three ledgers, and why it is not one flat table

An adversarial review refuted a flat six-bucket DWT split
(`tools/snes_survey/snes_device_dwt_design_adversarial_review.md`). Three
faults, each of which produces a *confident wrong answer* rather than a visible
failure:

| fault | consequence if ignored |
|---|---|
| the pacing wait is `__WFI()`, and the M7 gates the processor clock in sleep | the bucket we care about most reads ~0 → "pacing is free, we are compute-bound", the exact opposite of the truth |
| PPU and APU are *children* of `run_frame_events()` (`ppu_runLine` via `snes_handle_pos_stuff`; `snes_catchupApu` from inside `cpu_runOpcode` on any `$2140-$2143` access) | inclusive CPU + inclusive APU double-books every catch-up |
| DMA2D is asynchronous — `present_frame()` starts it, `snes_pcm_submit()` runs during it, `present_frame_wait()` drains the tail | charging its lifetime to a present bucket counts the same wall interval twice |

So:

- **Ledger A — foreground ACTIVE cycles (DWT).** Ten top-level, disjoint
  buckets: `framectl input rendarm emu* preskick pcm* prestail overlay swap
  pace_act`, plus `ACTIVE`. One DWT base per iteration, cumulative reads at each
  boundary, never a nested clear. **IRQ-inclusive** — see `irq_share`.
- **Ledger B — exclusive attribution inside the outers.** `ppu` (inclusive),
  `apu_lle` (exclusive), `core_rem`. Two coarse scopes injected into a
  *generated* copy of `external/sm/src/snes/snes.c`
  (`tools/snes_prof/instrument.py`; submodules are never edited). They are
  siblings, never nested, and a depth counter proves that every frame.
  **`core_rem` is not "the 65816"** — it is CPU + DMA + event scheduler + spin
  bookkeeping.
- **Ledger C — sleep-safe wall and audio deadlines.** TIM2, free-running,
  ~1 MHz, calibrated at boot. Not SysTick/`HAL_GetTick()` (stops in sleep, 1 ms
  resolution — and note `common_emu_frame_loop()` computes elapsed time from
  exactly that clock, so the shared frame loop's own timing is not sleep-safe
  either). Not `dma_counter` alone (16.625 ms per tick, too coarse to subdivide
  a 24.8 ms frame) — but `dma_counter` **is** the hardware reference TIM2 is
  validated against over the window, and what fps is computed from.

DMA2D lifetime and audio-DMA ticks are **side channels**. They are reported;
they are never summed into a ledger.

## Read the gates first

A run whose gates fail is not evidence. All of them print at the top of the
dump:

| gate | meaning |
|---|---|
| `tim2_cal` | TIM2 calibrated near 1 MHz — the wall clock is real |
| `wall_wfi` | **SLEEP-BLIND** (expected): DWT does not count through `__WFI()`, so `pace_act` is ISR time, *not* the wait — use `wall_pace`. **SLEEP-VISIBLE** means either a debugger is holding clocks on, or the loop never slept |
| `wall_vs_dma` | TIM2 agrees with the audio-DMA tick count over 64 frames |
| `ledgerA_mono` | no mark went backwards (no nested `CYCCNT` clear, no counter wrap inside a frame) |
| `ledgerB_nest` | scope depth never exceeded 1 and ended each frame at 0 — the sibling assumption held |
| `ledgerB_resid` | `emu_outer − ppu − apu` never went negative |
| `probe_cost` | measured cost of the ten Ledger A marks, as a share of ACTIVE |
| `irq_share` | aggregate upper bound on how much IRQ inflates every Ledger A bucket |

`scripts/check_snes_profile_wired.sh` is the other half, and it runs on every
link: it proves the probes are in the **binary**. The 32X device profile shipped
a run where the define never reached the objects — the dump looked right and
measured nothing.

## What this can and cannot decide

The dump ends with the judgement lines, recomputed from the fps it just
measured: the wall-cost saving required for **+3 / +5 / +10 fps**
(`1 − fps_base/fps_target`; at 40.3 fps that is 6.9% / 11.0% / 19.9%), against
the measured APU LLE exclusive share.

- **It can kill a candidate.** If `APU_LLE_exclusive + the CPU-side port/catchup
  cost the wire removes − the wire's own cost` is below the +5 line, an exact
  APU wire alone is NO-GO for +5.
- **It cannot adopt one.** LLE cost does not vanish, it becomes wire cost. A GO
  additionally needs an exact `$2140-$2143` port transcript, multiple scenes
  (field / dungeon / battle / menu / music transition), save-load identity, and
  a **profiler-OFF** device A/B.
- **Judge the tail, not the mean.** A large average pacing wait and a
  heavy p95/p99 compute-bound tail coexist routinely. The
  `deadline advance at pacing entry` histogram is the honest read: `0` means the
  frame arrived early and waited; `≥1` means the audio deadline had already
  passed and LLE never recovers that period.
- **The final A/B for any optimisation is run with the profiler off.** This
  build attributes cost; it is not the thing that measures the win.

## Repeating a run properly

Same ROM, same savestate, same scene, same clock/OC level, same scaling, same
volume, no debugger attached, at least three windows. Record which — the dump
prints clock, OC level, scaling mode and the drawn/skip split, but not the
scene.
