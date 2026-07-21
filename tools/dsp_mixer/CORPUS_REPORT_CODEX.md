# DSP block mixer corpus check (2026-07-21)

Command: `bash tools/dsp_mixer/run_ab.sh <rom> 1200 3` on a 20-title primary
corpus. Full logs are in `/tmp/codex-dsp-corpus-20260721/logs`.

## Result

- Before the hazard fix, 18/20 primary ROMs were executable by this generic
  core and 17/18 passed the full 1200-frame gate. Chrono Trigger exposed the
  ordering bug described below.
- Kirby Super Deluxe (SA-1) and Star Fox (SuperFX) died in `cart_readLorom`
  before capture, so they provide no mixer result.
- Five extra targeted probes (Earthworm Jim 1/2, Jurassic Park, Secret of
  Evermore, Plok) also passed 1200 frames.
- After the echo/BRR hazard fallback was added, all 23 core-executable probes
  passed: 23/23 bit-identical for Dsp state, 64 KiB ARAM, and 534 stereo
  samples over 1200 frames each.
- Chrono used reference fallback for only 3,234/640,800 samples (0.505%, 24
  chunks). The other 22 probes used zero fallback samples.
- For the 17 passing primary benchmarks, host ratios were min 1.02x, median
  1.17x, max 1.94x; aggregate replay time was 1075.9 ms reference vs 896.6 ms
  block (1.20x) before the fix. With the optimized constant-time hazard check,
  Chrono itself is 1.21x. A one-repetition 23-title rerun was 1.18x aggregate.
  These are host timings, not device FPS predictions.
- The non-diagnostic `mixer_block.o` grows by 456 bytes at `-Os` (2928 -> 3384
  bytes text; no data/BSS increase).

## Chrono Trigger failure

Enhanced `mixer_ab.c` chunk diagnostics locate the first failure at frame 326,
write-delimited segment 32, absolute DSP cycle 304 (36th sample of a 153-sample
segment). The pre-state has echo writes enabled, echo base `c100`, index 3049,
and 23 samples until wrap. Channel 2 is released/silent but continues BRR
decoding at `c107` then `c110`.

After echo wraps, the reference sample-major loop writes `c100`, `c104`,
`c108`, `c10c`, `c110`, ... before channel 2's next BRR decode. The voice-major
block mixer performs the BRR decodes before the chunk's echo writes, so it reads
old ARAM and its decode buffer diverges. Turning off the released-silent O(1)
path still fails at the identical sample, proving the problem is the general
voice-major/echo ordering. Reducing `CHUNK` from 256 to 32 merely moves the first
failure to frame 648; it is not a fix. `dspb_run(..., 1)` remains exact.

The fix represents the echo writes of each <=256-sample chunk as at most two
contiguous ring spans, conservatively walks the BRR decode chain (including DIR
loop-pointer reads), and calls the reference `dsp_cycle` only when the spans
overlap. This closes Chrono while retaining the block path everywhere else in
the corpus. The initial exact address-list checker also passed but reduced
Chrono to 0.68x; replacing it with constant-time ring-span membership restored
Chrono to 1.21x.

## Feature coverage

- Noise: Mortal Kombat II exercised `NON=ff` and passed 1200 frames. Secret of
  Evermore also exercised `NON=ff` and passed.
- Echo: DKC1/2/3, Mega Man X, Super Metroid, EarthBound, Zelda, FF6 and others
  exercised echo writes and passed; Chrono exposed the BRR-overlap hazard.
- KON/KOF: nonzero masks were exercised broadly and passed.
- PMON: no nonzero mask appeared in the first 1200 frames of the main corpus.
  Packy & Marlon reaches `PMON=02` by 4000 frames and passed the full 4000-frame
  gate (Dsp state + ARAM + samples). Its LoROM performs a legitimate open-bus
  read which this stripped core turns into `Die`; for this test only that read
  returned `openBus`, and the unrelated cart change was then reverted.

## Runtime integration

The validated mixer now lives in `external/sm/src/snes/dsp_block.c` and is
wired into the LLE APU path behind `SNES_DSP_BLOCK_MIXER` (default off).
`apu_run` accumulates DSP ticks across calls and catches up at actual ordering
boundaries: DSP register writes, SPC reads of ENVX/OUTX/ENDX, overlapping echo
reads, and SPC ARAM writes that overlap pending echo or BRR/DIR accesses. The
BRR/DIR dependencies are conservatively cached as 256-byte page bits once per
block; echo uses an exact ring-span test. Save/load and PCM drain explicitly
materialize pending DSP state.

M7 QEMU runtime gates used the shipping `SNES_DSP_MONO` path. Zelda, DKC, and
Chrono ran 1200 frames; Packy & Marlon ran 4000 frames to reach PMON=02. ON and
OFF matched both hashes in all four cases:

| ROM | Frames | STATEHASH | AUDIOHASH | OFF emu/apu insn/f | ON emu/apu insn/f |
|---|---:|---:|---:|---:|---:|
| Zelda ALttP | 1200 | `1d0d959d` | `4f118609` | 5,694,850 / 489,299 | 7,409,673 / 539,302 |
| DKC | 1200 | `d8132a15` | `566dcf5c` | 6,572,009 / 598,474 | 6,765,465 / 696,604 |
| Chrono Trigger | 1200 | `6c9ee7d2` | `878f36e1` | 5,907,696 / 744,031 | 6,645,775 / 837,384 |
| Packy & Marlon | 4000 | `f4d014fe` | `1b77ae03` | 6,993,093 / 757,580 | 13,608,015 / 813,327 |

Raw logs are in `/tmp/codex-dsp-runtime-20260721`. Packy required the same
temporary lower-LoROM open-bus harness workaround used by the offline PMON
gate; it was reverted after both runs. The M7 rig ELF text increased by 4,416
bytes at `-O2` (Zelda/DKC/Chrono); data and BSS were unchanged. The instruction
counts increased, especially for Packy's write-heavy driver. They are reported
as raw rig evidence only, not as device-FPS claims; the feature stays opt-in for
hardware measurement.
