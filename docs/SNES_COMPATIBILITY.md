# SNES ROM compatibility — a ~2,500-title library sweep

*Generated from `tools/snes_db/snes_analysis.sqlite` on 2026-07-16. Source set
`rpi5-2504`, 2,497 ROMs. All figures in this document come from SQL queries
against that DB — see `tools/snes_db/load.py` for the schema. Numbers are
reproducible: the queries live at the bottom of the file history for this
doc's commit; re-run them against a refreshed DB and expect the same shape.*

## What this is

This is a **host/rig simulation sweep**, not a device report. ~2,500 SNES
ROMs were run through the generic LakeSnes core (the one being ported to
Game & Watch as `explore/sfc-feasibility`) on two independent scanners:

- `tools/snes_survey` — boots each ROM for a fixed frame window on the host,
  counts lit framebuffer pixels, and fingerprints the sound driver uploaded
  to ARAM against a signature library (Nintendo's N-SPC and known third-party
  engines), recovering the N-SPC variant's song-list/instrument/DIR offsets
  where possible.
- `tools/snes_spin` — statically profiles the 65816 opcode stream for the
  same boot window and measures what fraction of executed gameplay opcodes
  are pure NMI-wait spin loops versus APU/IO-polling spin versus real work.

Both are **host emulation + static analysis**, cross-checked against QEMU M7
instruction counts for the one title (*A Link to the Past*) that has gone
all the way to a rig fps ladder. **insn count is not cycle count** — there is
no cache model, no bus-wait model, no DMA contention model in these numbers.
**The device is the final judge.** Every number below describes what the
simulation saw; none of it has been confirmed by flashing a device yet for
any title outside the existing Zelda/SMW/TMNT rig work already covered by
[`docs/HARNESSES.md`](HARNESSES.md) and the SNES feasibility initiative.

## 1. Boot compatibility

`sound_survey`, source_set `rpi5-2504`, 2,497 ROMs:

| Outcome | Count | % |
|---|---:|---:|
| **OK** — booted and rendered (`lit > 0`) | 1,816 | 72.7% |
| **UNRENDERED** — booted, no lit pixels in the sweep window | 463 | 18.5% |
| **BOOT_CRASH** — core rejected or crashed on load | 218 | 8.7% |
| Total | 2,497 | 100% |

`BOOT_CRASH` is **not** a uniform bucket. It is dominated by carts LakeSnes'
current mapper support can't run at all — enhancement-chip titles (SA-1,
SuperFX, Cx4, S-DD1) that the survey's `LOAD_FAIL`/crash paths both
land in as `BOOT_CRASH` in this table. **DSP-1 was in that list when this
survey ran and no longer is** — `dsp1_hle.c` implements it, and Mario Kart,
Pilotwings, Suzuka 8 Hours and Battle Racers all boot and run. **Cx4 left the
list next** — a clean-room HLE shipped in `4063dade` (2026-08-15), and both of
its carts have since booted and played on a device (2026-08-17: *Mega Man X2*
title → gameplay at 52 fps on the launcher counter; *Mega Man X3* intro
live-rendering, 48,277 of 153,600 framebuffer bytes changing per probe
capture). Its siblings
DSP-2/3/4 share DSP-1's romType and are now refused by title rather than
attached to the wrong HLE (see "What each chip would actually take" below).
Confirmed examples from this sweep:
*Star Fox* and *Super Star Fox Weekend* (SuperFX), *Mega Man X2* and
*Mega Man X3* (Cx4, built for exactly those two carts). Not every
`BOOT_CRASH` title is confirmed against a specific chip — some are
core-specific crashes still being triaged, so treat the count as "won't run
today," not "definitely needs unimplemented silicon."

`UNRENDERED` means the ROM booted without crashing but drew nothing the
survey's pixel counter caught inside its frame window — either the title
needs longer than the window to reach a lit screen, needs a button press
past a splash/BIOS screen the survey doesn't send, or draws through a path
(e.g. a mode/layer combination) the counter misses. It is a "needs a longer
or interactive look," not a compatibility verdict.

## 2. Sound-driver family

Classified with `tools/snes_survey/classify.py`'s signature logic (strong
engine-identifying signatures, weak shared idioms set aside) over the 1,816
rendered ROMs:

| Family | Count | % of rendered |
|---|---:|---:|
| **N-SPC** (Nintendo's engine and its studio forks) | 1,391 | 76.6% |
| UNMATCHED (no signature hit) | 296 | 16.3% |
| WEAK_ONLY (only shared idioms hit, engine unidentified) | 68 | 3.7% |
| Custom engines (Capcom, SoftCreat, Neverland, Prism, Konami, AsciiShuichi, Rare, Falcom) | 61 | 3.4% |
| Total rendered | 1,816 | 100% |

Custom-engine breakdown (all non-N-SPC, sig-identified): Capcom 17,
SoftCreat 14, Neverland 7, Prism 6, Konami 6, AsciiShuichi 5, Rare 4,
Falcom 2.

### N-SPC sub-variant — what drives native-audio HLE eligibility today

The survey recovers per-ROM ARAM offsets (song list / instrument table /
sample DIR) using VGMTrans's per-variant recipe, tagged `nspc_params` in the
DB (`nspc:v=<variant>,...`). Over the 1,391 rendered N-SPC ROMs:

| `nspc_params` variant | Count | % of N-SPC |
|---|---:|---:|
| `std` | 679 | 48.8% |
| `SMW` | 537 | 38.6% |
| `-` (offset recovery failed) | 156 | 11.2% |
| `GD3` (Konami) | 15 | 1.1% |
| `YI` (Yoshi's Island) | 4 | 0.3% |

The `-` bucket isn't unclassifiable — cross-referencing it against
`classify.py`'s signature-name variant detector (which reads the matched
signature *names*, not the recovered offsets) resolves most of it: 118 are
Tose-pattern (`YSFR` signatures), 34 are standard-pattern hits where offset
recovery just didn't score a plausible song list, 2 are Falcom (Ys IV), 2 are
SMW-era. So the full N-SPC variant picture spans **std / SMW / GD3 / YI /
Tose / Falcom / other forks** — exactly the studio-fork sprawl N-SPC is known
for.

**Covered vs. fallback today:** `std` + `SMW` + `GD3` + `YI` = 1,235/1,391
N-SPC titles (88.8%) have a param-recoverable variant a native player can be
handed directly; the remaining 156 (11.2%, mostly Tose-pattern) fall back to
the interpreted SPC700 path until a Tose offset recipe is added. Of that
covered set, **`std` and `YI` (683 ROMs, 49.1% of N-SPC)** are the two
variants proven end-to-end on the rig today (see §5, §6) — `SMW` and `GD3`
have recovered offsets but haven't had a native player validated against
them yet, so this report keeps them out of the "proven" tier below.

## 3. Speed-lever headroom — spin-skip

`spin_sweep`, source_set `rpi5-2504`, over the 1,792 ROMs that table itself
rendered in its own boot window (a separate scan from `sound_survey`'s 1,816
— the two tools use different frame windows and occasionally disagree on a
title; see caveats):

| `pure_spin` bucket | Count | % |
|---|---:|---:|
| 0–10% | 504 | 28.1% |
| 10–30% | 125 | 7.0% |
| 30–50% | 222 | 12.4% |
| 50–70% | 435 | 24.3% |
| 70–90% | 407 | 22.7% |
| 90%+ | 99 | 5.5% |

Median `pure_spin` = **52.1%**. **941/1,792 (52.5%) sit at ≥50% pure spin** —
the big winners for the idle-skip lever, where more than half of every
executed gameplay opcode is a CPU sitting in an NMI-wait loop that a
hint-gated skip can fast-forward for free. The 0–10% tail (504 ROMs, 28.1%)
is the other end: action titles that poll the APU or an I/O port tightly
enough that there's no idle time to find — the skip lever buys them nothing,
and forcing it costs (§7).

## 4. Compatibility tiers

Synthesized per-ROM from both tables (`sound_survey` boot status +
`nspc_params` variant, `spin_sweep` `pure_spin`), over all 2,497 ROMs:

| Tier | Definition | Count | % |
|---|---|---:|---:|
| **A — 60fps-track** | bootable, N-SPC `std`/`YI` (audio-HLE-eligible), `pure_spin` ≥50% | 362 | 14.5% |
| **B — runs well** | bootable, has one lever (spin ≥50% *or* audio-HLE-eligible) but not both | 786 | 31.5% |
| **C — runs, baseline** | bootable, no lever available yet | 668 | 26.8% |
| **D — unsupported** | `BOOT_CRASH` (enhancement-chip class) | 218 | 8.7% |
| **E — needs investigation** | `UNRENDERED` in the sweep window | 463 | 18.5% |
| Total | | 2,497 | 100% |

Representative titles (English name shown where the ROM carries a bilingual
label; full 2,497-row breakdown lives in the gitignored
`tools/snes_db/rom_tiers.csv`, not committed — see Constraints below):

- **Tier A**: *The Legend of Zelda: A Link to the Past* (BS re-releases),
  *Super Metroid*, *Super Mario World*, *EarthBound*, *Kirby Bowl*,
  *Dragon Quest III*, *Ogre Battle: The March of the Black Queen* (BS ver.)
- **Tier B**: *Chrono Trigger*, *Donkey Kong Country*, *Donkey Kong Country 2*,
  *Street Fighter II*, *F-Zero*, *Wild Guns*, *ActRaiser 2*,
  *Illusion of Gaia*, *Bahamut Lagoon*, *Front Mission*
- **Tier C**: *Donkey Kong Country 3*, *Gradius III*, *Terranigma*,
  *Front Mission: Gun Hazard*
- **Tier D**: *Star Fox*, *Super Star Fox Weekend* (SuperFX), *Mega Man X2*,
  *Mega Man X3* (Cx4)

## What each chip would actually take — and which reason closes it

Worth writing down because the reason on record was **wrong**. These were
carried as "unrealistic in the RAM_EMU budget", and that is not what stops
them. The SNES core is an *overlay*: `STM32H7B0VBTx_SDCARD.ld:481` captures
`build/snes/*.o` into `.overlay_snes` (`dsp1_hle.o` included) and it is
streamed off the card into RAM_EMU, so **coprocessor code never touches the
256 KB internal flash image**. Measured on the shipped ELF: `.overlay_snes`
94,800 + `.overlay_snes_bss` 412,760 = 507,560 of 741,376, leaving
**233,816 bytes free**. Both the chip's code and the cart RAM it needs come
out of that one number.

| chip | RAM_EMU (code + cart RAM) | CPU per frame | what actually closes it |
|---|---|---|---|
| **SA-1** | ✗ BW-RAM is 256 KB by itself | ✗ a second 65816 | both |
| **SuperFX** | ✓ ~114 KB | ✗ a RISC CPU interpreted per frame | **CPU, not RAM** |
| **Cx4** (HLE) | ✓ ~33 KB, shipped in `.overlay_snes` | ✓ intercepted commands, ~free | **done — `4063dade`, device-verified** |
| **DSP-2/3/4** (HLE) | ✓ ~16 KB | ✓ same shape as DSP-1 | nothing; not written yet |
| S-DD1 / SPC7110 | ✓ ~30 KB | ⚠ decompression inside DMA windows | unmeasured, borderline |
| OBC1 / S-RTC | ✓ ~10 KB / ~1 KB | ✓ register intercepts | nothing; very few titles |

So the honest list of *closed* chips is **SA-1 and SuperFX**, and SuperFX is
closed on CPU alone. **Cx4 was closed for a reason that was never true, and it
is done now** — shipped as a clean-room, docs-based HLE (external/sm 5bc1605,
no snes9x lineage; bit-identical to a behavioral reference on both carts at
900/1800/3600 frames) in `4063dade`, and **device-verified 2026-08-17**:
*Mega Man X2* boots to its title and plays (52 fps on the launcher counter),
*Mega Man X3* renders its intro live. The two Tier D Cx4 titles above are no
longer a floor. That leaves S-DD1/SPC7110 as the only unmeasured rows.

⚠️ The CPU column is **estimated, not measured**. The one measured data point
is our own: DSP-1's HLE costs essentially nothing, which is what makes the
DSP-2/3/4 and Cx4-HLE rows credible and says nothing about S-DD1 or SuperFX.
Before starting any of these, price it the way this project prices everything
else — by ablation on hardware, not by the table above.
- **Tier E**: *Secret of Mana*, *Doom*, *Super Street Fighter II: The New
  Challengers*, *ActRaiser*, *Live A Live*, *Tales of Phantasia*

## 5. The fps ladder — what the levers actually buy (Zelda: A Link to the Past)

From `measurement`, the M7-rig instruction-count ladder for *A Link to the
Past* (`Zelda-ALttP`, same statehash across rows — same save state measured
at each stage):

| Config | Total insn | fps | What changed |
|---|---:|---:|---|
| stock (hard-float) | 7,590,000 | 44.8 | pre-optimization baseline |
| + PPU opt | 6,906,000 | 49.2 | PPU renderer −49% insn (committed `snes-perf@31ce2e6`) |
| + 65816→C translator | 5,976,000 | 56.9 | static recompile, 100% native coverage, no interpreter fallback |
| + spin-skip | 5,616,000 | 60.5 | 68.4% of ops skipped via WRAM spin replay (translator+spinskip) |
| **+ audio HLE (full stack)** | **4,639,861** | **73.3** | native N-SPC player live-wired, APU window 600K→195K insn, translator+spinskip+audioHLE together, 0 interpreter fallbacks |

Side branches measured independently (not chained above): audio-HLE-only on
top of the translator (no spin-skip) reached 5,925,000 insn / 57.3 fps —
confirming most of the win comes from spin-skip, with audio HLE compounding
on top rather than replacing it. **Net: PPU-opt's 49.2fps → full-stack's
73.3fps**, the "rig 49→73fps" result this initiative has been chasing.

## 6. Honest caveats

- **Simulation, not device.** Everything above is host emulation (boot/sound
  survey), static 65816 opcode analysis (spin sweep), or QEMU M7 instruction
  counts (the Zelda ladder). None of it models cache, bus wait states, or DMA
  contention, and instruction count is not cycle count. The device has not
  run this library; it is the only judge that matters for a ship decision.
- **No ROM filename list is committed.** Per repo policy, per-ROM identity
  stays off the public repo. This document carries aggregate tables and a
  handful of representative titles only; the full 2,497-row classification
  is written to `tools/snes_db/rom_tiers.csv`, which `tools/snes_db/.gitignore`
  excludes.
- **Enhancement chips**: DSP-1 (four carts, 2026-08-14) and Cx4 (both titles,
  2026-08-17) are implemented and device-verified; DSP-2/3/4 are refused by
  title; S-DD1 and SA-1 are unimplemented; SuperFX is infeasible on this MCU
  (established by the earlier `tools/snes_harness` initiative closed in
  `docs/HARNESSES.md`). Tier D is down to the SuperFX titles.
- **The spin hint-gate is a prerequisite for shipping the skip lever, not an
  optional polish pass.** The rig also measured the failure mode:
  *TMNT: Tournament Fighters* (`TMNT-IV`) has no skippable spins, and running
  the plain spin-skip tracker against it anyway cost **27.6 fps vs. 38.2 fps
  stock — a 27.7% regression** from tracker overhead alone (`measurement`
  row `spinskip-overhead`). Ungated, the lever that gives Zelda +8fps takes
  TMNT-class titles backward. A per-title (or per-PC-site) hint gate has to
  ship before spin-skip goes wide.
- **The two per-ROM tables don't fully agree with each other.** `sound_survey`
  says 1,816 ROMs rendered; `spin_sweep`'s own boot pass says 1,792 rendered,
  319 crashed, 386 unrendered — different frame windows, different code
  paths, occasionally different verdicts on the same title. Tier
  classification in §4 treats `sound_survey` as the canonical boot-compat
  signal and only asks `spin_sweep` for a `pure_spin` number when that
  table's own status for the same ROM is `OK`.
- **`spin_gate`** (the per-ROM safety-gate sweep that would validate the
  hint-gate's verdict against each ROM in the library) is empty in this DB
  snapshot — that sweep is still running. This report has no per-ROM
  gate-verdict summary yet; that's the next section to add once it lands.
