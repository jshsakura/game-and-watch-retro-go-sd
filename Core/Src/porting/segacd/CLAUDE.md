# Sega/Mega CD port — playbook

Status: **IN PROGRESS.** This is a large multi-session port (PCE-CD took several).
The feasibility research (memory `segacd-feasibility`) established the path; the
user directed a full build. This file is the durable plan so any session can
continue. **For the live boot state + next task, read memory
`session-handoff-0716-segacd` first** — it's ahead of this file.

## Boot sequence — how the BIOS logo actually works (0717, all runtime-verified)

The whole boot is a chain of two-CPU handshakes. As of 0717 every stage runs
except the final drive-status gate. Host harness: `tools/segacd_harness/boot_test.c`
(build in the handoff memory), interleaves the sub per scanline. Disassemblers:
`/tmp/{m68kdis,subdis,ramdis}.py`.

1. **Both 68Ks boot.** Main from region BIOS at $0; sub released by the main
   clearing SRES ($A12001). Byte-order: PRG-RAM uses the `^1` swap (see
   `main_prgwin_read8`); Word-RAM too — a `uint16*` of an even offset reads the
   logical BE word, identical to GPGX's `word_ram2M`.
2. **CD data read.** Sub drives the CDD (Read-TOC/Play/Seek), CDC decodes sectors.
   KEY: the sub-BIOS CDC position gate only sets WRRQ (buffer-write) when the head
   is **1-4 sectors BEFORE the target**, so `CD.cur_lba` is SIGNED and Play/Seek
   starts at `lba-3` with NO clamp; `segacd_cd_update` feeds the decoder EVERY
   play tick incl. pre-target/pregap sectors (zeroed data, correct HEAD). Data
   reaches the sub via **DTRG dest=3 host-read** ($FF8008), not Word-RAM DMA.
3. **Main↔sub comm is LOCKSTEP.** The BIOS logo handshake needs intra-frame
   ping-pong (main rings $A12000 doorbell → sub L2 ISR acks $FF800F bit1 → main
   grants DMNA $A12003 bit1). Our engine therefore **interleaves the sub per
   scanline** (`md_scanline_frame` / on device: needs a segacd-specific frame
   loop, NOT a change to the shared `gwenesis_md_frame`). Running the sub once
   per whole frame deadlocks the toggle handshake.
4. **Word-RAM GFX ASIC** (`segacd_gfx.c`, ported from `pd_cd/gfx.c`): a $FF8066
   write triggers `segacd_gfx_start`, which single-shot renders the rotated/scaled
   image from stamps+trace-vectors into the Word-RAM image buffer and arms a
   frame-paced level-1 (INT1) completion interrupt (gated on IEN1 $FF8033 bit1).
   The sub's animation loop (0x7a06) blocks on the frame counter its INT1 handler
   (0x7ace) toggles. **This is done and verified** (renders every frame, no crash).
5. **The stamps are drawn by the MAIN, not the sub** (GPGX-confirmed). The main
   copies BIOS-ROM $c608/$db60/$db08 → Word-RAM $200080/$201880/$220608 at main PC
   0x1b00, reached via the **$FFFDA8 RAM trampoline** (a `4EF9 <addr>` the BIOS
   installs; it flips to `jmp $1eda` at the logo frame). The per-frame Word-RAM→
   VRAM DMA is $1eda, indexed by Word-RAM $219E00 (the sub's frame counter) via
   the table at $1f66. Gated by the `$fe26` VBlank semaphore + $A12003 RET.

### THE remaining gate (next task)
The main's **top-level boot-mode machine ($FFFDDA, dispatcher 0x05c2) is stuck in
mode 4 (disc-detect, 0x1d58)**. It climbs mode 4→8→**0x10 (LOGO)** only when
`$FFFE20 & 0xF0 != 0` (gate 0x1d8e) then `$FFD007` bit7 = disc-present (0x2244).
**Our $FFFE20 = 0.** The controller port $A10003 (read by the VBlank ISR
0x1162→0x1180 into $FE20) correctly returns 0x7f (idle), so $FE20 is the processed
**drive/controller-status mirror**, and our CDD status path never sets its high
nibble. Find what writes $FFFE20 / $FFD0xx (relay 0x4c28 copies $D0xx→Word-RAM)
and make the CDD report drive-ready-with-disc. Then everything above runs and the
logo renders. Harness prints a `BOOT-MODE` line with $FFFDDA/$FFFE20/$A10003.

## The one-line architecture

**gwenesis (base Mega Drive, unchanged) + PicoDrive's `pico/cd/*` hardware layer
(adapted) + a SECOND 68000 run as a `m68ki_cpu_core` context on gwenesis's own
Musashi.** Do NOT import PicoDrive's base emulator or its Cyclone/FAME 68K — keep
our device-tuned gwenesis. Only the CD *hardware* (gate array, CDC, CDD, Word-RAM
ASIC, PCM, sub-CPU scheduling) comes from PicoDrive.

## Why this shape (settled — do not relitigate)

- Base MD core is device-tuned and already fits `RAM_EMU`; PicoDrive is GP2X-tuned
  (64 MB, ARM asm). Grafting CD onto gwenesis is the lightest path.
- The second 68K costs ~2 KB RAM, not a code image: this gwenesis Musashi keeps all
  state in one global `m68ki_cpu_core m68k` whose **`memory_map` is part of the
  struct**. Swap contexts by `memcpy`-ing the whole struct in/out (PicoDrive's
  `m68k_get/set_context` model, `pd_cd/sek.c`). Sub-CPU gets its own memory_map
  pointing at PRG/Word RAM — exactly the hardware model.

## RAM plan — SCORCHED EARTH (the hard part, from research)

Simultaneously-resident writable RAM ≈ **976 KB** (CD 840 + MD base 136). Fits only
by:
1. **XIP ALL emulator code from external flash** (like `sm.xip`): the MD overlay
   code (584 KB) + the CD layer code go to an `SEGACD_CODE` region at a sentinel
   ORIGIN, relocated by `store_file_in_flash_relocate()`. Frees `RAM_EMU` for data.
2. **Single framebuffer** (320×240×2 = 150 KB), not the 300 KB double-buffer pool.
3. **Recruit every bank**: PRG-RAM 512 KB + Word-RAM 256 KB → AXI; PCM 64 KB → AHB;
   BRAM 8 KB → wherever. MD 68K RAM/VRAM/Z80 in the remainder.
- Margin is ~0. **RAM-cart (128 KB backup) games are OUT of scope** — they overflow.
- PRG-RAM is NOT pageable from SD (live sub-CPU work RAM; research verdict final).
  What streams from SD is the **disc image** (172 KB/s, trivial — reuse PCE-CD path).

## Why CDD/CDC/gate-array logic is hand-written, not compiled from `pd_cd/` (0720, settled — do not relitigate)

`Core/Src/porting/segacd/pd_cd/` still holds the vendored PicoDrive/Genesis-Plus-GX
CD layer (`cdd.c` 1,231 lines, `cdc.c` 897, `mcd.c` 482, `memory.c` 1,359 — 5,556
lines total). **None of it is in the Makefile's segacd source list** — the CDD/CDC/
gate-array logic in `segacd_cd.c` (1,296 lines) is a from-scratch reimplementation,
using `pd_cd/*.c` purely as a read-only behavioral reference (every non-trivial
branch in `segacd_cd.c` cites the exact `pd_cd/cdd.c:LINE` it mirrors). The
"Integration seams" table right below this section describes the *original* plan —
symbol-rebinding `pd_cd/*.c` onto gwenesis — from the phase-1 scaffold. That plan
was abandoned without ever recording why, which is a documentation bug: it let two
real regressions ship through the hand-port (CDD response BCD encoding used the
wrong convention, `c07d403b`; the CDD tick ran at 60Hz instead of the real 75Hz,
`28e4c03f`) while this file kept describing "rebind and use `pd_cd/cdd.c` directly"
as if it were still the live plan. Both bugs were only caught by byte-for-byte
diffing our engine's trace against a PicoDrive run — see `session-handoff-0716-segacd.md`
0720 nights 8-10 — exactly the kind of check a straight compile-and-rebind would
have made unnecessary, which is why this question (why not just build `pd_cd/`?)
matters and needed a real answer instead of an assumption.

**The answer, checked 0720 (numbers, not impression):**

1. **`pd_cd/*.c` cannot currently compile in this tree at all.** Its own includes:
   `../pico_int.h` (PicoDrive's core header — not vendored, would need PicoDrive's
   entire `pico/` tree, not just `pico/cd/`), `../pico_cmn.c` (a **.c file textually
   `#include`d**, not linked — `mcd.c` shares a translation unit with PicoDrive's
   base-Genesis rendering/memory code), `../sound/ym2612.h`, `megasd.h`,
   `libchdr/cdrom.h` + `libchdr/chd.h` (CHD compressed-image support — a whole
   separate third-party library we don't use, we go bin/cue via FatFs), and
   `tremor/ivorbisfile.h` (Ogg Vorbis decoder — CD-DA here uses PCM via our own
   `segacd_audio.c`, not Vorbis). None of these exist in this repo. This is not "a
   few missing headers" — it is PicoDrive's entire emulation core.
2. **Even fully vendored, `Pico_mcd` (the struct every `pd_cd/*.c` line reads/writes
   through) doesn't fit `RAM_EMU` on its own.** It's one fixed-layout blob:
   `bios[0x20000]` (128 KB — PicoDrive copies the whole region BIOS into RAM; we XIP
   it straight from flash, zero RAM cost) + `prg_ram[0x80000]` (512 KB) +
   `word_ram2M[0x40000]` plus its 1M-mode sibling in the same union (~384 KB with
   padding) + `pcm_ram[0x10000]` (64 KB, bundled in the *same* struct as PRG/Word-RAM
   — we specifically put PCM-RAM in AHB SRAM via `ahb_malloc()`, *outside*
   `RAM_EMU`, an escape valve `Pico_mcd`'s single-struct layout has no way to use) +
   misc/PCM-channel state. **Total ≈1,096 KB against a 724 KB budget — 51% over,
   before any code, before the base PicoDrive `Pico`/`PicoMem` struct (VRAM/CRAM/
   VSRAM) these files also assume, before CDC/CDD working state beyond `mcd_state`
   itself.**
3. **The CD-specific files pull in PicoDrive's own execution model, not just CD
   logic.** `pd_cd/*.c` reference 24 distinct PicoDrive-core symbols beyond
   `Pico_mcd` itself: `pcd_event_schedule`/`pcd_event`/`pcd_run_events` (PicoDrive's
   own cycle-accurate event scheduler — how it gets the CDD tick to interleave with
   68K execution at a precise cycle position, the actual mechanism behind the 0720
   night-13 finding that our engine batches CDD ticks after a whole frame instead),
   `pcd_run_cpus`/`pcd_run_cpus_lockstep`/`pcd_run_cpus_normal` (PicoDrive's own
   main-loop orchestration of both 68Ks), `SekCyclesDone`/`SekPc`/
   `SekShouldInterrupt` (PicoDrive's own Musashi wrapper — a **second, separate**
   68K core from the gwenesis Musashi we already run the base MD emulation on).
   Satisfying the "Integration seams" table's rebind list for these means
   reimplementing PicoDrive's own scheduler and CPU-run loop against gwenesis's
   timing model — at which point the result is not "`pd_cd/cdd.c` with symbols
   rebound," it is a rewrite of `pd_cd/cdd.c`'s call sites, which is what
   `segacd_cd.c`/`segacd_engine.c` already are.

**Conclusion**: hand-writing was the only viable path given `RAM_EMU` and the
decision (documented above, "Do NOT import PicoDrive's base emulator") to keep
gwenesis as the only 68K core in the overlay — not an oversight, not laziness.
**The actual lesson is process, not architecture**: a hand-port against a reference
implementation needs byte-level diff testing against that reference *as a matter of
routine*, not just careful reading — both 0720 bugs were structurally reasonable
code that was simply wrong in a way structural review didn't catch, and only a
literal trace comparison did. See memory `rule-cross-reference-before-trusting-own-chain`.
`pd_cd/` stays in the tree as that reference corpus (same role as
`retro-go-stm32/components/odroid/` for the launcher — read for behavior, not
built) — do not delete it, do not try to wire it into the Makefile without
addressing all three points above first.

## Integration seams (where PicoDrive's cd layer meets gwenesis) — HISTORICAL, describes the abandoned symbol-rebind plan, see section above for why it wasn't taken

The `pd_cd/*` files assume PicoDrive's `Pico`/`PicoMem` structs, `SekCycleCnt`, its
own `m68k_*`, and endianness helpers (`genplus_macros.h`). Each must be rebound to
gwenesis:

| PicoDrive symbol | gwenesis equivalent to bind |
|---|---|
| main 68K exec / cycles | `m68k_run()`, `m68k.cycles` (gwenesis m68kcpu.c) |
| sub 68K context | second `m68ki_cpu_core` snapshot + memcpy swap |
| `Pico.mcd` (mcd_state) | new `segacd_state` we own (PRG/Word/PCM/bram/gate regs) |
| main-CPU bus into CD space | extend gwenesis_bus.c memory_map for $A12000 gate array, $020000 PRG window, Word-RAM |
| Word-RAM ASIC (`gfx.c`) | runs against our Word-RAM buffer; invoked on GA op start |
| PCM (`pcm.c`) | mix into gwenesis audio submit path |
| CDD/CDC sector I/O (`cdd.c` `pm_read/pm_seek`) | splice FatFs (odroid_sdcard) — bin/cue/iso only, NO chd |

## Phased plan

- **Phase 1 (now)**: scaffold. Porting dir, vendored `pd_cd/`, `segacd_state`
  struct + RAM allocation, dual-68K context swap engine, build wiring (Makefile,
  `.overlay_segacd` + `SEGACD_CODE` XIP region, `create_sd_data`, `flash_sd`).
  Goal: compiles and links, sub-CPU steps against PRG-RAM. CD state machine stubbed.
- **Phase 2**: gate array ($A12000 regs) + PRG-RAM banking + Word-RAM 2M/1M
  arbitration in gwenesis_bus.c. Main↔sub handshake (giveWord/returnWord).
- **Phase 3**: CDD/CDC state machine (`pd_cd/cdd.c`,`cdc.c`) + FatFs sector stream
  (bin/cue). BIOS load (region BIOS files on SD, like other CD systems).
- **Phase 4**: Word-RAM graphics ASIC (`pd_cd/gfx.c`) + RF5C164 PCM (`pd_cd/pcm.c`)
  + CD-DA streaming/mix. BRAM save (reuse gwenesis_sram pattern).
- **Phase 5**: device bring-up, per-title debugging. Frameskip tuning (research says
  the dual-68K load likely needs it).

## Speed strategy (THE priority — analysis says ~1.8-2.3x MD, so frameskip + skips are mandatory)

Levers, most-effective first. Build them in from the start; do not bolt on later.

1. **Sub-68K idle-skip** (biggest). The sub spends most cycles spinning on a GA
   status reg waiting for the main / CDD. `segacd_bus.c` counts unchanged reads
   of the same reg → `SCD.sub_idle`; `segacd_run_sub` then skips the slice; any
   GA/CDD write calls `segacd_poll_wake()`. Correctness rule: EVERY state change
   the sub could wait on must wake it, or it hangs. Same lever as GBA/VB/WS.
2. **Main-68K idle-skip.** The main also polls (BIOS wait loops, giveWord). Add
   the same poll/wake on the main GA reads.
3. **Frameskip.** `common_emu_frame_loop()` already gates draw; on skip frames
   run CPUs+CDD but drop VDP render AND the Word-RAM ASIC blit.
4. **Coarse interleave.** Context-swap main<->sub a few times per frame, not per
   scanline — each swap is two struct copies. Sync only at CDD/GA events.
5. **ASIC on-demand only.** Run `gfx.c` when a GA op is started, never per frame;
   its cost is O(pixels), so heavy roto (Sonic CD, Silpheed) is where fps dies —
   those may stay frameskip-heavy or unsupported.
6. **Batch audio.** PCM + CD-DA generated once per frame into the mix buffer, not
   per sample.

Watch-out: XIP code from external flash is a *speed cost* (slower fetch than
internal). Keep the hottest inner loops (68K dispatch) in internal flash/RAM if
the budget allows; XIP the cold CD/BIOS code.

## Test path

- Host: extend `linux/Makefile.wswan`-style two-process cold-boot harness for CD.
- Instruction budget: `tools/m7_qemu_rig/rig_md.c` is the MD **denominator** (built,
  runs on QEMU M7). Next `rig_mcd.c` = same + second 68K context → the multiplier
  that gates real-time. Both already scoped.
- ROMs for MD baseline: `game-and-what/.../roms/md/*.md` (101 present, raw big-endian).

## Vendored files (untracked, phase-1 scaffold)

`pd_cd/`: mcd.c memory.c cdc.c cdd.c cell_map.c gfx.c gfx_dma.c pcm.c sek.c
cd_parse.c + headers. From `notaz/picodrive` `pico/cd/` @ the clone in scratchpad.
Licenses: PicoDrive is MAME-derived / its own license — keep headers, add to
attribution before any release.

**Not in the Makefile and not meant to be — reference corpus, not a build target.**
See "Why CDD/CDC/gate-array logic is hand-written, not compiled from `pd_cd/`"
above for the full reasoning (incomplete dependency closure, RAM footprint ~1.1MB
vs 724KB budget, coupled to PicoDrive's own CPU/scheduler). Keep it in the tree for
behavioral reference (every hand-ported branch in `segacd_cd.c` etc. cites a
`pd_cd/*.c:LINE`) — same role as `retro-go-stm32/components/odroid/`.
