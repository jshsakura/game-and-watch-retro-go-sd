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

## Integration seams (where PicoDrive's cd layer meets gwenesis)

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
