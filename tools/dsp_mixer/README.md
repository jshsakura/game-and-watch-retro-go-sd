# S-DSP block mixer (PoC)

`mixer_block.c` re-implements the S-DSP sample loop voice-major instead of
sample-major: per run of samples between DSP register writes, each voice is
mixed in a tight per-voice loop (state in locals, per-chunk constants hoisted,
released-silent voices skipped ahead decode-to-decode in O(1)), then a mixdown
+ echo pass. **Bit-identical to `external/sm/src/snes/dsp.c`** — proven, not
assumed, by `mixer_ab.c`.

Why exactness is possible: KON/KOF apply inside `dsp_write` (block boundaries
by construction); PMON reads the previous voice's same-sample output (voices
run in order, rows buffered); the noise LFSR is independent (precomputed per
chunk); accumulation clamps are identity for all-zero rows.

## A/B harness

`mixer_ab.c` runs the real emulation (survey event loop), records per frame a
DSP snapshot + ARAM + the cycle-tagged register-write stream (apu.c compiled
with `-Ddsp_cycle=hook_dsp_cycle -Ddsp_write=hook_dsp_write`), then replays
every frame offline through the reference loop and the block mixer on identical
inputs and compares **full Dsp state + 64 KB ARAM (echo writes) + all 534
samples**. On mismatch it bisects to the exact sample and dumps both states.

```bash
bash tools/dsp_mixer/run_ab.sh <rom> [frames=1200] [bench-reps=3]
# MIXER_ENGINE=ref|blk limits the bench phase to one engine
```

## Results (2026-07-15, host)

| ROM | gate (1200 frames) | ref ns/sample | block ns/sample | ratio |
|---|---|---|---|---|
| Zelda ALttP (title, 6–7 dense voices) | **bit-identical** | 132.3 | 114.2 | **1.16×** |
| DKC (echo-heavy, more idle voices) | **bit-identical** | 93.7 | 64.3 | **1.46×** |

No feature forced a per-sample fallback (PMON/noise/KON/KOF all exact). One
real bug the gate caught: a BRR end-block (`previousFlags==1`) releases the
voice *inside* the decoder; the voice loop's local state clobbered it — fixed
by mirroring the release into the locals.

## Notes

- The echo unit is the irreducible serial core (FIR state + ARAM traffic every
  sample, by SNES design) — it bounds the dense-voice ratio.
- Integration home: the sound-HLE path (`SpcPlayer_GenerateSamples`), which
  already calls the DSP in 64-sample runs. The LLE path interleaves `dsp_cycle`
  with SPC700 opcodes every 32 APU cycles — batching there needs lazy catchup
  (run blocks, catch up on SPC700 reads of ENVX/OUTX/ENDx), bounded follow-on.
- M7 rig A/B pending (rig busy at time of writing); host OoO hides dispatch
  costs the in-order M7 pays, so host ratios are likely conservative — but that
  is a projection, not a measurement.
- perf hardware counters unavailable on this box (`perf_event_paranoid=4`);
  timing ratios only.
