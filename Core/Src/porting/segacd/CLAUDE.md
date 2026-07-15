# Sega/Mega CD port — playbook

Status: **IN PROGRESS, phase 1.** This is a large multi-session port (PCE-CD took
several). The feasibility research (memory `segacd-feasibility`) established the
path; the user directed a full build. This file is the durable plan so any
session can continue.

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
