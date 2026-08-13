# rc (65816 static recompilation) — 2026 feasibility re-verification

Written 2026-08-10. Investigation only — no code modified, no device flashed. The
two binding constraints from `docs/RESUME_GNW.md §3` are re-verified against the
**current tree**, which has shipped an ITCM-based rc and invalidates most of §3's
assumptions. Findings marked **unknown** are genuinely unknown — they need a
device measurement this doc does not perform.

## TL;DR

The original RESUME_GNW §3 framing ("32 MB flat map → DTCM mandatory; 1-2 M
XIP calls/sec → scale unverified") is **obsolete**. The shipping design:

- Stores the dispatch hash in **overlay BSS** (`RAM_EMU`), not DTCM. For SMW's
  270 sites it is **~5 KB** (not 85 KB; that figure was the old 8371-site design).
- Runs translated sites from **ITCM** (`0x00000000–0x0000FFFF`, 0-wait-state),
  not XIP from external flash. The XIP-call-frequency concern is **not relevant
  to the shipping design**.
- The 270-site ITCM subset is **already in the tree and untested on device**.
  Both constraints (A and B) are answered for the shipping case. The questions
  that remain are (1) does it activate, (2) what's the device fps delta, and
  (3) would scaling to the full 8371-site map via XIP still be viable.

The doc you should read first is `docs/SNES_NEXT_SESSION.md` for the stall-bound
rules; this doc only covers rc.

## What shipped

Code (off by default, opt-in via `RCSMW=1`):

| File | Role |
|---|---|
| `external/sm/src/snes/rc_dispatch.c` (88 lines) | Per-bank Knuth open-addressing hash. `rc_dispatch_init(storage, bank_off, bank_mask, addrs, nsites, fns)`, `rc_dispatch_lookup(bank, pc)`, `g_rc_active` flag. **Zero heap.** |
| `external/sm/src/snes/rc_dispatch.h` | API + `rc_entry_t = {uint16_t pc, uint16_t id}` (4 B). |
| `Core/Src/porting/snes/rc_smw_sites.c` (223 lines) | SMW 270-site compilation unit. Links at ITCM VMA. Defines `rc_hash_storage[]`, `rc_bank_off/mask[]`, `rc_smw_header` (discovery metadata). |
| `Core/Src/porting/snes/main_snes.c:787-853` | `rc_smw_activate()`: SMW title FNV-1a gate → code-region FNV-1a gate → `rc_dispatch_init()`. |
| `external/sm/src/snes/cpu.c:217,256` | `cpu_runOpcode` fast path: `if (g_rc_active) { id = rc_dispatch_lookup(...); if (id) { rc_fns[id-1](cpu); continue; } }`. |
| `generated/rc_smw/rc_sites.inc` (auto) | 270 native site functions + `rc_fns[]`, `rc_addrs[]`, `rc_site_lens[]`, `RC_NSITES=270`, `RC_HASH_CAP=768`, `RC_CODE_HASH=0xda713b69`. |
| `generated/rc_smw/cpu_copy.c` (auto) | Byte-for-byte copy of `external/sm/src/snes/cpu.c` with init/reset/free symbols renamed so they don't clash with the overlay's. `--gc-sections` discards the unused ones. |
| `STM32H7B0VBTx_SDCARD.ld:496-503` | `.itcm_rc_hot` section at `ORIGIN(ITCMRAM)+0x4`. Build-time `ASSERT` ITCM overflow. |
| `Makefile:140-177`, `Makefile.common:909-913, 1800-1825` | RCSMW build wiring. |
| `tools/sfc_recomp/` | Host PoC translator (`translate.py`, `build.sh`) that produces `rc_sites.inc` and `cpu_copy.c` from `smw.sfc`. |

The design change in two lines: the original plan was **8371 sites, XIP from
external flash, dispatch in DTCM**. The shipped design is **270 hot sites, ITCM
direct-link, dispatch in overlay BSS**. Same principle (per-bank Knuth hash,
"the bytes are identity" code-region gate), very different scale and execution
domain.

## Constraint A — dispatch map placement: **RESOLVED, no longer binding**

The original concern was that a flat `pc → site*` map of the SNES 24-bit address
space needed ~32 MB and would not fit anywhere. The shipped design avoids it with
per-bank hash tables (`rc_entry_t = {uint16_t pc, uint16_t id}`):

```
rc_hash_storage[RC_HASH_CAP]   // 768 × 4 B = 3 072 B  (SMW 270 sites)
rc_bank_off[256]               // 256 × 4 B = 1 024 B
rc_bank_mask[256]              // 256 × 4 B = 1 024 B
                               // ─────────────────────
                               // total     = 5 120 B
```

These are placed in `.overlay_snes_bss` (RAM_EMU), not DTCM. The compile-time
budget assert (`RC_DISPATCH_BUDGET_BYTES = 96 * 1024`) is at `rc_smw_sites.c:188`.
SMW uses ~5 KB; the headroom is enormous.

`docs/RC_DISPATCH_ANALYSIS.md` derives the 212 KB free margin in RAM_EMU after
all overlays and BSS for the *old* 8371-site hash. For the 270-site shipping
design the dispatch hash is 17× smaller, so RAM_EMU fit is not a question.

**Conclusion:** Constraint A is resolved for SMW. For scaling to other ROMs,
each new ROM adds its own `rc_<rom>_sites.c` with its own dispatch table; the
RAM_EMU budget assert catches overflow at link time. No DTCM, no 32 MB map, no
heap. The RESUME_GNW §3 "DTCM mandatory" conclusion was wrong for the shipping
design — the actual solution moved the table out of DTCM entirely.

## Constraint B — XIP call frequency: **RESOLVED for the shipping 270-site design, OPEN for scaling**

The shipping design uses **ITCM**, not XIP. The 270-site compiled subset lands
directly at `ORIGIN(ITCMRAM)+0x4` and is copied there by `run_internal_emu`
before `app_main` runs. Zero wait-state, no QSPI, no I-cache miss. This is the
same mechanism the 32X used for its SH-2 interpreter (device-measured +30% fps
at unchanged instruction count — see `STM32H7B0VBTx_SDCARD.ld:519-522`).

So the "1-2 M indirect XIP calls/sec" concern from RESUME_GNW §3 does not apply
to today's design. The XIP question is only relevant for the **scale-up**: would
moving to the full 8371-site map (the design that produced the −42.3 % rig
instruction count) be viable when ITCM holds only 270?

The full map is 1.1-1.5 MB of code — cannot fit in 64 KB ITCM. Scaling to it
would require either:

1. **XIP from external flash** via the SM-style QSPI cache (this is what
   `rc_probe.c` Stage 2 measures). The cache hit rate then determines viability.
2. **Hot/cold split inside ITCM** — keep the top 270 in ITCM, page in cold sites
   from external flash on demand. Much more complex; would need a working-set
   survey first.
3. **Per-bank ITCM residency** — keep all sites for the 3-4 hottest banks in
   ITCM, XIP the rest. The bank distribution is in `tools/sfc_recomp/smw_hot_sites.csv`.

The **shipping 270-site ITCM design is a separate measurement** from the
**8371-site XIP scale-up**. Both are open on device. The probe that answers the
scale-up question is `RC_PROBE=1` Stage 2 (see "Procedure to measure" below).

## rc_probe — Stage 1/2/3 on-device probe

`Core/Src/rc_probe.c` (626 lines), gated by `RC_PROBE ?= 0` (`Makefile:62-66`).
When `RC_PROBE=1`:

- `rc_probe.c` is compiled, `-DRC_PROBE=1` is added to C_DEFS.
- `rcprobe.xip` (2 KB, 64 sentinel-linked XIP sites) is extracted via
  `objcopy --only-section=.xip_rcprobe` (`Makefile.common:2306-2307`).
- `rg_main.c:1198` calls `rc_probe_run_if_requested(boot_buttons)` after
  `sdcard_init`.
- Runtime gate: hold **B_GAME | B_TIME** (`RC_COMBO`) at boot.
- Reports to LCD + serial `printf`, then halts in a watchdog-safe loop.

### Stage 1 — map lookup cost (CONSTRAINT A, mostly moot)

COLD-cache cost of `rc_dispatch_lookup` across placements (DTCM / AHB / flash),
schemes (hash / bsearch), N (4355 / 8371), and access patterns (uniform /
locality). 100 K lookups each. Reports cycles/lookup. **Hash N=8371 skipped**
(16384 slots × 6 B = 96 KB > 81 KB DTCM heap and AHB heap). Bsearch N=8371 fits
in 50 KB.

For the shipping design this stage is mostly historical — the 270-site hash in
overlay BSS answers placement at link time. Still useful for the scale-up
question (what's the per-lookup cost when N grows and placement moves).

### Stage 2 — XIP call cost (CONSTRAINT B, the binding measurement)

Functional XIP test: caches `rcprobe.xip` into QSPI flash via
`odroid_overlay_cache_file_in_flash_relocate` (same mechanism as `sm.xip`).
64 site functions linked at `RCPROBE_CODE` sentinel (`0xD0D00000`), each with a
unique addend to defeat `-fipa-icf` merging. Sites called via
`real_fns[i] = rc_xip_table[i] + xoff` (true indirect `blx`). 1 M calls.

Two passes:

- **COLD**: invalidate I+D cache before the loop. Average cycles/call includes
  I-cache miss + QSPI fetch latency.
- **WARM**: I-cache hot. Average cycles/call is the steady-state XIP cost.

**Cold-warm delta = I-cache miss penalty per call.** This is the number that
determines whether the 8371-site XIP scale-up is viable on a stall-bound chip.

Correctness check: each per-site counter must equal `m/K`. A wrong counter is a
DOOM-hazard signal — codegen broke.

### Stage 3 — combined dispatch lower bound

Hash lookup (DTCM, the winning placement from Stage 1) + RAM-resident stub site
call + return. 1 M calls. **No flash veneer** (sites in RAM). `per_op` =
cycles/opcode. Compared against the SNES budget (~240-480 cyc/op total).

This is a **lower bound** on dispatch cost — the shipping ITCM design should
beat it because ITCM is 0-wait-state and the stub here is in regular RAM.

## Procedure to measure

### Build 1: probe-only firmware (RC_PROBE=1)

```
rm -rf build    # RC_PROBE toggle requires clean (C_DEPS gap)
make release DOCKER=1 RC_PROBE=1 COVERFLOW=0 CHEAT_CODES=0 \
    SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
    INTFLASH_BANK=2 ZH_CN=0 ZH_TW=0 KO_KR=0 JA_JP=0
```

Outputs:

- `build/gw_retro_go_intflash.bin` (~253 KB, fits the 256 K bank)
- `sd_content/roms/homebrew/rcprobe.xip` (2 KB, copy to SD `/roms/homebrew/`)

Flash the intflash image, copy the xip, hold **GAME + TIME** at boot. Read the
LCD and the serial printf log. Three tables (Stage 1, 2, 3) print and the device
halts.

### Build 2: shipping SMW rc (RCSMW=1)

```
rm -rf build
make release DOCKER=1 RCSMW=1 COVERFLOW=0 CHEAT_CODES=0 \
    SHARED_HIBERNATE_SAVESTATE=1 DISABLE_SPLASH_SCREEN=1 \
    INTFLASH_BANK=2 ZH_CN=0 ZH_TW=0 KO_KR=0 JA_JP=0
```

Outputs:

- `build/gw_retro_go_intflash.bin`
- `sd_content/cores/snes.bin` (includes the `.itcm_rc_hot` payload appended after
  the SNES overlay per `Makefile.common:2282`)

The release build is what would ship. To measure device fps, run the deterministic
Zelda3 900-frame savestate window from `docs/SNES_NEXT_SESSION.md` against this
build and against the `RCSMW=0` baseline. **Always `cmp` the two arms'
`snes.bin` first** — `FLAGS_STAMP` did not record the SNES define groups until
2026-08-10, so toggling a knob rebuilt nothing and both arms were the same
binary.

Note: `RCSMW=1` excludes `SNES_SMW_HLE=1` and `SNES_NSPC_HLE=1` (Makefile
`$(error ...)` at lines 142, 145). The rc fast path replaces the SMW spin-skip
path entirely and replaces the interpreter that the generic-wire N-SPC fallback
depends on. These are mutually exclusive feature combinations.

### Build 3: host A/B rig (correctness gate)

This is the QEMU-Cortex-M7 rig (`tools/m7_qemu_rig/run_snes_rc.sh` +
`run_snes.sh`), not the device. **The four hashes (state, audio, both framebuffer
windows) MUST be bit-identical to the interpreter baseline**; if any hash moves,
the rc translator has a bug and the device test is invalid.

```
# Prereq: generate per-ROM translator output
bash tools/sfc_recomp/build.sh

# Then A/B (RIG_WINDOW=40 to avoid boot skewing the average):
RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes_rc.sh external/smw/smw.sfc 120   # rc
RIG_WINDOW=40 bash tools/m7_qemu_rig/run_snes.sh   external/smw/smw.sfc 120    # interp
```

**Caveat:** the host rig uses the *old 8371-site* design with bank-table
dispatch (`rc_dispatch_hash.c`), not the *shipping 270-site* ITCM subset. The
−42.3 % instruction count this rig reports is the full-map ceiling, not the
shipping subset. The stall-bound rule (`docs/SNES_NEXT_SESSION.md` "The rule")
also applies: rig instruction deltas transmit to device fps at roughly 1/3,
sometimes less.

## Open questions (genuinely unknown until device measurement)

1. **Activation.** `rc_smw_activate()` requires the loaded SMW ROM to satisfy
   two FNV-1a gates:
   - Title hash `0xFB0BD0EC` (LoROM title at `0x7FC0`, 21 bytes).
   - Code-region hash `0xda713b69` (FNV-1a of consumed bytes — opcode + operands
     — at all 270 site PCs).
   The expected code hash is baked into `generated/rc_smw/rc_sites.inc` as
   `RC_CODE_HASH`. If the user's SMW ROM has different bytes at any translated
   site PC, the gate rejects it and rc stays off — the build runs, but rc is a
   no-op. `docs/RC_ACTIVATION_VERIFY.md` says `0x5a04e964`; **that is stale**;
   the current value in the tree is `0xda713b69`. The diagnostic `printf` on
   mismatch goes to serial — the only way to confirm activation is to read it.

2. **Device fps delta of the 270-site ITCM subset.** Could be anywhere from 0
   (sites never hit; the hot subset was wrong) to ~5-10 % of the CPU axis
   (most opcodes hit; ITCM eliminates interpreter dispatch overhead). The CPU
   axis is 24 % of the frame today, so the optimistic ceiling is roughly
   24 % × 10 % = 2.4 % of frame ≈ +1.3 fps at 52 fps.

3. **Stage 2 XIP call cost.** This is the binding measurement for scaling to
   8371 sites. **Unknown until `RC_PROBE=1` runs on device.**

4. **8371-site scale-up viability.** The host shows −42.3 % instruction count.
   Stall-bound transmission at ~1/3 puts the device ceiling near −14 %, or
   roughly +5 fps at 52. But "rig −X → device maybe −X/3" is a maybe, not a
   promise — and on a stall-bound chip, XIP I-cache misses may eat most of the
   win (see rc-XIP history: −42 % rig → **+0 device** in `OPTIMIZATION_LEDGER`,
   but that was full XIP with no ITCM for hot code; the 270-ITCM + 8000-XIP
   split is a different design). Stage 2 + a real A/B is the only honest
   answer.

## What the doc does NOT do

- Does not run `RC_PROBE=1` build. **Code modification forbidden by the task**;
  building from current tree on the main worktree would conflict with the
  shared `build/` directory with the user. The build commands above are the
  procedure, not a result.
- Does not run `RCSMW=1` device A/B. **User does device measurement.**
- Does not modify `../gnw-apu` worktree (kept as-is per `SNES_NEXT_SESSION.md`).
- Does not delete the stale `docs/RC_ACTIVATION_VERIFY.md` code hash. That's a
  one-line edit, but it is a code change and the task forbids it. Flagged here.

## Cross-references

- `docs/RESUME_GNW.md §3` — original feasibility (pre-shipping, XIP/DTCM).
- `docs/RC_DISPATCH_ANALYSIS.md` — 212 KB RAM_EMU margin proof (8371-site).
- `docs/RC_PRIOR_ART.md` — static recompilation + XIP prior art survey.
- `docs/SNES_NEXT_SESSION.md` — today's handoff (52.36 fps baseline, stall-bound
  rules, 7 closed levers). Read this before any device work.
- `docs/HARNESSES.md` — full harness catalog.

## Stale claims in older docs (flag, do not fix here)

- `docs/RC_ACTIVATION_VERIFY.md` says `RC_CODE_HASH = 0x5a04e964`. **Stale.**
  The current value in `generated/rc_smw/rc_sites.inc` is `0xda713b69`. The
  activate path is correct (uses the header's value, not the doc's); only the
  doc is wrong.
- `docs/RESUME_GNW.md §3` conclusions about DTCM-mandatory and XIP-scale apply
  to the *pre-shipping* 8371-site design, not the shipping 270-site ITCM design.
  The constraints themselves are still relevant for the scale-up question, but
  they are no longer binding on whether rc can ship today.
- `docs/SNES_PEAK_FRAME_ANALYSIS.md` shift-on-reuse section has a separate stale
  claim about `renderBuffer` being one line — flagged in `SNES_NEXT_SESSION.md`.
