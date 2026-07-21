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

Worktree edits are the minimal hazard fallback in `mixer_block.c`, diagnostic
counters/chunk bisection in `mixer_ab.c`, the harness-only diagnostic define in
`run_ab.sh`, and this report. The `external/sm` submodule is clean.
