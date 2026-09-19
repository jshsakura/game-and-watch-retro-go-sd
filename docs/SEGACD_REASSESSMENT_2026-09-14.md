# Sega CD stock-hardware reassessment (2026-09-14)

## Decision

The removed Sega CD port must not be restored in its final 2026-07-24 form.
That build used only 128 KiB of the hardware's 512 KiB PRG RAM and sent every
access above it through a one-byte `fopen`/`fseek`/`fread` or `fwrite` cycle on
`/tmp/scd_prg.bin`. It never displayed a verified frame on the device.

The earlier, stronger conclusion that stock hardware can *only* work with
external PSRAM is no longer proven. It assumed both of the following:

1. the 300 KiB double framebuffer remains reserved; and
2. PRG RAM must be one contiguous 512 KiB host allocation.

The 35 Hz beam-race work may remove the first assumption. The second is an
emulator implementation choice: gwenesis maps the sub-68K address space in
64 KiB pages, so the eight PRG pages can have separate host backing pointers.
Under those two conditions, the historical custom gwenesis-based core has a
tight but arithmetically feasible all-SRAM placement. This is a new phase-0
feasibility result, not evidence that Sega CD currently boots or performs well.

## What actually existed

The last complete tree is tag `testbed-full-20260724-1447` at commit
`0904992e`. It contains:

- a gwenesis main 68K/Z80/VDP/YM2612 base;
- a second gwenesis Musashi context for the sub-68K;
- custom gate-array, CDD/CDC, Word RAM graphics, RF5C164 PCM and BRAM code;
- `/roms/segacd/*.cue` launcher registration;
- region BIOS lookup at `/bios/segacd/bios_CD_{U,E,J}.bin`;
- cold gwenesis code and tables in `/cores/segacd.xip`.

The current `testbed` tree has no `APPID_SEGACD`, source list, linker overlay,
launcher registration or dispatch. Commit `6a6c9729` removed them. The current
PicoDrive submodule still contains its mature Mega CD implementation, but it is
not wired as a Game & Watch Sega CD target.

The historical bring-up fixed real defects: main-68K reset before BIOS mapping,
missing-BIOS return into a destroyed launcher, audio DMA never starting, and a
missing `lcd_swap()`. None of those commits proved device gameplay. Issue #31's
owner close-out records that the screen never appeared on a device and that
earlier apparent success was host-harness output.

## The invalid 128 KiB compromise

The final header says:

```c
#define SEGACD_PRG_RAM_SIZE (128 * 1024)
```

The sub-68K's `$020000-$07ffff` PRG range was implemented as file-backed
handlers. Each guest byte access opened `/tmp/scd_prg.bin`, sought, transferred
one byte and closed the file. CDC DMA and savestate loops also used the reduced
`SEGACD_PRG_RAM_SIZE`, so the design did not preserve general 512 KiB PRG
semantics.

The reduction came from one 3000-frame Detonator Orgun profile. All four banks
were selected once during initialization while sampled sub-68K execution stayed
in bank 0 during that run. That is useful title data; it is not a platform-wide
proof that three banks can be removed.

## Reproduced final build and exact memory use

The tag was checked out in an isolated `/tmp` worktree with its exact submodule
revisions and rebuilt successfully using the historical release flags and
`SEGACD=1`. The resulting `gw_retro_go.map` reports:

| Item | Bytes | KiB |
|---|---:|---:|
| Sega CD resident overlay code | 19,112 | 18.664 |
| Sega CD overlay BSS | 156,212 | 152.551 |
| Static AXI total | 175,324 | 171.215 |
| `RAM_EMU` capacity, double framebuffer | 741,376 | 724.000 |
| AXI left after the static overlay | 566,052 | 552.785 |
| AHB allocator capacity in that build | 87,904 | 85.844 |
| DTCM libc heap, stack excluded | 90,232 | 88.117 |

The old allocator names are misleading. `ahb_calloc()` tries `ram_malloc()`
first, and `ahb_malloc()` also prefers AXI once AXI allocation has started. The
actual launch allocation sequence was therefore:

| Allocation | Pool | Bytes |
|---|---|---:|
| Main 68K RAM | ITCM | 65,536 |
| Unused-for-CD Genesis cart SRAM | AHB | 65,536 |
| YM2612 tables | AXI | 47,104 |
| Reduced PRG RAM | AXI | 131,072 |
| Word RAM | AXI | 262,144 |
| PCM RAM | AXI | 65,536 |

This leaves 60,196 bytes in AXI and 22,368 bytes in AHB. Restoring the missing
384 KiB of PRG RAM without changing the layout therefore misses by 333,020
bytes. A single framebuffer alone recovers 153,600 bytes and still misses by
179,420 bytes. The old issue was right that a framebuffer change alone is not
enough.

## New all-SRAM placement enabled by the 35 Hz premise

One RGB565 320x240 framebuffer consumes 153,600 bytes, leaving 894,976 bytes of
AXI SRAM. With the historical 175,324-byte overlay, 719,652 bytes remain for
dynamic Sega CD state.

AHB currently starts its global allocator at `0x300088a0` because 34,976 bytes
are permanently reserved for GBA BIOS, cheats and sound buffers. Emulator cores
are mutually exclusive, so Sega CD can reuse the same physical addresses through
a core-specific AHB overlay/start pointer while leaving GBA's own layout intact.
That exposes the full 120 KiB below the 8 KiB audio-DMA reserve to Sega CD.

A concrete placement is:

| Pool | Sega CD contents | Used | Remaining |
|---|---|---:|---:|
| AXI, after one framebuffer and static overlay | Word RAM 256 KiB + seven PRG pages 448 KiB | 720,896 | -1,244 |
| AHB, core-specific 120 KiB | PCM 64 KiB + YM tables 46 KiB | 112,640 | 10,240 |
| DTCM heap area | one PRG page 64 KiB | 65,536 | 24,696 |
| ITCM | main 68K RAM 64 KiB | 65,536 | 0 |

AXI is short by only 1,244 bytes in that first placement. Moving one existing
4,800-byte CDDA mix buffer from overlay BSS into the remaining AHB space closes
it, leaving approximately:

- AXI: 3,556 bytes;
- AHB: 5,440 bytes;
- DTCM heap: 24,696 bytes.

The total uncommitted margin is 33,692 bytes. Alignment, FatFs/newlib use and
current-tree growth must still be charged against it. It is narrow, but it is a
real static-RAM layout and contains the full 512 KiB PRG RAM. No SD paging or
flash writes are involved.

The PRG representation must become `page[8]`, one pointer per 64 KiB guest page.
The historical source has only a small set of direct PRG references: sub-CPU map
construction, main-bank handlers, CDC DMA, reset and savestate. Those paths must
all use page helpers; retaining a fake contiguous pointer would recreate the old
bug. Word RAM can remain one contiguous 256 KiB AXI allocation.

## Why current PicoDrive is not a drop-in replacement

A host compile against the current submodule measured:

```text
sizeof(mcd_state) = 1,128,256
sizeof(PicoMem)   =   139,648
sizeof(Pico)      =     1,496
```

`mcd_state` embeds a 128 KiB BIOS copy, 512 KiB PRG RAM, a 384 KiB Word RAM
union layout, 64 KiB PCM RAM and the remaining CD state. The BIOS can be XIP and
the Word RAM padding can be redesigned, but the structure must then be split
across MCU banks and many PicoDrive assumptions changed. Its CPU scheduling and
CD code are also coupled to the PicoDrive execution model. The current
PicoDrive code is the behavioral oracle; integrating it directly is a separate,
larger port than repairing the custom gwenesis layer.

## Required phase-0 gate

Do not resume CDC/handshake debugging first. Prove the changed memory premise on
the current tree:

1. land and device-validate the 35 Hz one-framebuffer path with zero observed
   beam races;
2. add a `SEGACD_RAM_PROBE` linker-only target, without the emulator, containing
   the exact static overlay plus full PRG/Word/PCM/YM allocations above;
3. make AHB allocation/overlay bounds core-specific so Sega CD can reuse the GBA
   addresses without changing GBA behavior;
4. assert every bank end at link time and assert at runtime that at least a
   defined DTCM/newlib margin remains;
5. exercise read/write/checksum over all eight non-contiguous PRG pages on the
   device;
6. only after those gates pass, port the archived core forward and restart boot
   debugging with device breadcrumbs from frame zero.

 Status 2026-09-16, gates 4+5 PASSED on device. The runtime probe
 (`segacd_ram_probe_run_if_requested`, dispatched from rg_main after the SD
 mount) reported through logbuf + LCD: `G4 PASS` (page 7 malloc'd from the DTCM
 heap at 0x20006858, 24,696-B margin claim alongside it succeeded) and `G5
 14/14` — all eight PRG pages (prg0-6 in AXI at 64-K strides, prg7 in DTCM),
 main-68K RAM (ITCM, full 64 K), Word RAM, static BSS, PCM/YM/CDDA (AHB)
 passed two-round pattern write/verify plus FNV-1a checksum. Non-contiguity is
 device-proven: prg6 = 0x2408f23c (AXI) vs prg7 = 0x20006858 (DTCM). One
 operational note: the boot-combo latch (`boot_buttons`, main.c:521) read 0 on
 every button-held reset although GPIOC-IDR showed both pins low and the rc_probe
 precedent used the same combo in July; the run was triggered instead by halting
 the running launcher over SWD and resuming at the probe entry with r0=0xC0.
 the combo-latch anomaly is unexplained and worth revisiting before any
 button-triggered tooling relies on it. Gate 1's eye verdict also landed
 (same day): **35 Hz bands on this panel; 33 Hz (N5/R24) is visually clean**
 but costs the 32X core its frame rate (12 fps @ CPU 87%, OSD readout) — so
 the single-FB design should target 33 Hz, and only for the Sega CD session;
 the panel-rate arm is not evidence about single-FB performance either way
 (that is gate 6 bring-up). Remaining for phase 0: gate 6 (port the archived
 core forward).

 Status 2026-09-16: gate 2 linked (margins AXI 3,556 / AHB 5,440 / ITCM 64 KiB
exact / DTCM 90,236 >= 90,232), gate 3 landed (stateless
`ahb_set_core_base()`), and the gate 4+5 runtime half is implemented in the
probe and host-verified by `tests/test_segacd_ram_probe.c` (trigger gate,
page-7 + margin claim sequence, 14-region sweep, report). The device run —
power on while holding GAME+TIME, then read the drawn report or the logbuf —
is the remaining evidence for gates 4 and 5. The 35 Hz eye verdict (gate 1's
last piece) is also still pending a user looking at the panel.

**Status 2026-09-16: gate 2 passed.** `SEGACD_RAM_PROBE=1` (link-only; section
placeholders sized from the rebuilt tag, `Core/Src/segacd_ram_probe.c`) links
the full placement above on the current tree with the reassessment margins
reproduced: AXI 3,556 free, AHB 5,440 free, ITCM 64 K exact, DTCM heap
90,236 ≥ 90,232. The overlay-BSS placeholder had to be 151,412, not 156,212 —
the CDDA buffer is charged to AHB here, and leaving it in the static BSS too
fails the AXI assert by exactly 4,800. Gate 1's device run is on hardware
(p35 arm; 13.4 fps under the observation-only swap-race counter); the panel
eye verdict is still pending. Gate 3 landed the same day: `ahb_set_core_base()`
(stateless AHB bump-pool rebase; ceiling invariant; boot `ahb_init()` restores
the global default) — the probe's own DTCM assert rejected the first stateful
version, which is the gate working as designed: an 8-B `.bss` static costs 24 B
of a heap margin that is 4 B. Flag-off cost is +72 B (assert string only); the
probe re-passes with the gate-2 margins. Gates 4+5 passed on device the same
day (see the status block above).

**Status 2026-09-19: gate 6 — the core boots from disc on device.** The
archived core is forward-ported onto the probe placement (`a0a85e35` plus the
bring-up fix stack; see the ledger's Sega CD section for the full chain:
staging destination, sentinel patch range, audio-symbol separation, GA register
file 0x200 with $FF8200+ unmapped, AHB static tail, RS1 subfield + DSR re-arm
CDD protocol, NODISC tray probe, single-FB lock). On hardware with a
track-1-only image the BIOS completes its disc sequence — tray probe, Read TOC,
Play, RS0=READY — and the main CPU enters the disc boot path with the panel
rendering. Gameplay, audio and the performance campaign remain open; the
35 Hz/33 Hz single-FB session work is now a runtime lever of this core rather
than a phase-0 precondition (the eye verdict rejected the panel-rate fix for
the 32X core, and this core owns its framebuffer aliasing).

The phase-0 result can still fail if the current 35 Hz path cannot safely expose
the second framebuffer's memory, current resident usage consumes the margin, or
the remaining DTCM heap is insufficient. In that case external OSPI PSRAM is the
clean general solution. The previous 128 KiB plus SD-file paging path remains
closed regardless of the result.

## Stale records

- `docs/SEGACD_INVESTIGATION.md` ends with **GO** and says an HLE jump fix is
  enough. It describes a host-harness point in time and was superseded.
- `website/blog/2026-07-20-sega-cd-the-sub-cpu-that-waited-for-a-dead-main.md`
  says Sega CD boots and Sonic CD plays. The later device close-out says this was
  never observed.
- `docs/OPTIMIZATION_LEDGER.md` and GitHub issues #31/#36 contain the later
  device truth, but their “external PSRAM only” claim predates the 35 Hz
  framebuffer premise and treats PRG contiguity as mandatory.

