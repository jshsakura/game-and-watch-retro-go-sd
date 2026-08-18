# 32X — where today (2026-08-18) left it

Goal as stated: **Doom 32X playing without lag.** This file is what a new session
needs so it does not re-derive any of it. The queue and the closed-axis ledger
stay where they are (`32X_NEXT_SESSION.md`, `32X_CLOSED.md`); this is the day's
delta and the one open blocker.

## The number that matters

Doom gameplay is **~17 drawn fps against a 60 Hz machine — about 28% of real
speed**, and that is the lag. Closing it needs **~3.5x**.

Everything measurable was measured today, and the total of every remaining axis
is about **2x**, most of it unbuilt:

| axis | ceiling | state |
|---|---|---|
| C interpreter cost/instruction | x1.5 | doc arithmetic (92 -> 45 device cycles is its optimistic floor) |
| hand-written Thumb-2 SH-2 core | **~x1.0** | SNES precedent *measured*: the SPC700 Thumb-2 engine, 2,230 lines of asm, bought **+0.5 fps** |
| dynarec | **impossible** | no Thumb-2 (Cortex-M) backend exists anywhere; ledger, twice re-litigated |
| ssh2 spin, 17.1% of frame | **not available** | it is the sound service loop — proven today, see below |
| msh2 SDRAM poll, 23.4% of msh2 guest | x1.13 | parking built and rejected before (detect cost > spin saved) |
| hot leaf HLE (GBR getter) | ~x1.08 | 4 instructions, ~1,977 calls/frame, 11.3% of guest |
| R_DrawColumn HLE | x1.22 (est.) | **cannot be measured yet** — see blocker |
| compositor / MD VDP draw | x1.02 / x1.05 | already optimised; priced today |
| OC 340 MHz, forced-draw 1-in-1 | applied | +12%, x3.7 drawn |

D32XR is the only multiplicative unknown left, and its 41.4 fps figure is now of
**unknown subject** — see `32X_NEXT_SESSION.md` queue 0.

## The blocker, and it gates everything above

**The rig cannot reach gameplay, so every 32X guest profile ever taken here is of
the attract demo.** That includes the "R_DrawColumn is 36.5% of msh2" figure the
HLE plan rests on — that came from a *device* profile of a savestate-resumed
scene, and nothing offline has ever reproduced it.

**The proof that none of the reachable states is gameplay: `188 unique PCs`, in
every one of them.**

| state | unique guest PCs | guest insns in window |
|---|---|---|
| pure attract (no pad script) | **188** | 20,963,667 |
| old pad script | **188** | 6,638,320 |
| A-mash, menu over a rendered level | **188** | 8,385,479 |

A Doom frame that renders a 320x224 view through R_DrawColumn touches *thousands*
of distinct master-SH-2 PCs. 188 is a small fixed loop set — and it is the same
188 in all three, with the top entries carrying identical shares to two decimals.
The 3D image behind the menu is a still the game drew once; the compositor runs
every frame (`lines=224`), so the screen looks alive while the SH-2 is producing
no new pixels.

So the distinction is not attract-versus-gameplay. **All three states are the same
front-end wait loop, and the rig has never once profiled Doom rendering.** Any
guest-side optimisation argued from these profiles — including the hot leaf
functions below — is argued from the wrong program.

Today's pad-script work got as far as the main menu drawn on top of a rendered
level (121 colours, 97,198 guest SH-2 insn/frame against attract's 69,879, peak
frame 23.9M host insn — which matches the ledger's "heavy frame = 24.4M device
cycles"). The renderer runs there. The menu never commits.

What was tried and does not work, so nobody repeats it:

- **START is Doom's pause/menu toggle.** Any script that keeps pressing it cycles
  the front end; alternating START and A landed back on the TITLE at f430.
- Pressing exactly twice (title, then commit) never leaves the title.
- **B and C are inert**: an A-only run and an A-then-B-then-C run give identical
  frame counts to seven digits, from different binaries.
- A-mash reaches the menu and holds it.

> **CORRECTED, later the same day.** The rig *can* reach gameplay — the runs
> were simply too short. `tools/pico_host` (native, ~250x the rig) found it in
> seconds: with the A-mash pad script **Doom's first level starts at frame 620**,
> and every rig run above stopped at 450-560. Run the rig past f700 with
> `-DRIG_PAD_SCRIPT` and it is profiling gameplay. The savestate loader below is
> still the better long-term answer (it reproduces the *device's* exact scene),
> but it is no longer the blocker.

**The clean fix is a savestate loader in the rig, not more input timing.**
`rig_32x.c` already has `RIG_STATE_CAP` / `RIG_STATE_TEST` / `RIG_STATE_WARMUP`
and the build already compiles `pico/state.c`. The device card has
`/data/32x/Doom.32x-0.sav`; **it is not on this machine**. Get that file here and
the rig can resume the exact scene the device measures, forever, offline. That
single step unblocks the R_DrawColumn question and every gameplay A/B after it.

## Landed today

- **Audio hashing in the rig** (`dfa8678d`). The framebuffer hash cannot hear,
  and the last spin-skip attempt was reverted for breaking Doom's gunshot PWM
  effect — found by ear on hardware after a rig A/B called it clean.
- **Why idle-skip breaks the sound**, mechanism (`ce0c3b6e`): a parked SH-2 wakes
  only on an internal IRQ; `pwm.c` has no wake and the VBlank poll_event excludes
  `SH2_STATE_SLEEP` by name. Doom's slave SH-2 **busy-polls the PWM mono FIFO**
  instead of waiting on its interrupt, so parked it never refills. Waking on FIFO
  movement restores the baseline audio hash *exactly* — and the gain collapses
  from -17.7% to -0.4%. ssh2's 17.1% is sound work, not idle spin.
- **Five levers re-priced by ablation, all already done or capped** — fastloop
  filter (removing it is 3.5x worse), BF/S countdown collapse (already
  implemented), packed-pixel run detector (+22% without it), MD VDP layers (-5.5%
  only by deleting visible content), idle-skip. Switches left behind:
  `GNW_SH2_NO_FASTLOOPS`, `GNW_PP_NO_RUNDET`, `GNW_MD_ABLATE`.
- **>4 MiB carts bank now** (`037fe7aa`, picodrive `eeca1e72`): one missing call,
  40 bytes of overlay. The official 5 MiB D32XR release loads for the first time.
- **`RIG_FB_DUMP` + `fbdump_to_png.py`** — read a frozen screen instead of
  guessing. This is what showed the gate had been passing a death screen.
- **GATE3 fixed** — it accepted blank -> still as "moving", which is the shape of
  a fatal error; D32XR sat on `Z_Malloc: failed on 496` through a whole campaign
  of GATE3 PASSes.

## Two traps worth carrying forward

- **md5 both arms before believing an A/B.** Two A/Bs today showed a delta of
  exactly zero because the arms were the same binary — once from a define that
  was already unconditional, once from a handler that already existed earlier in
  the same function.
- **grep before implementing.** A hand-written BF/S countdown collapse turned out
  to sit twenty lines above where it was being added.


---

# Device deployment — what happened 2026-08-18 evening

The console was stuck and "controls did not work". None of it was the firmware.

**Flashing works, and it is done from `rpi-genie5`, not from here.** This VM is
QEMU: `lsusb` shows only virtual devices, there is no USB passthrough, and no
probe can ever be attached. The RPi5 has an **ST-Link/V2** and `gnwmanager` at
`/home/pi/.local/bin` (⚠️ not on the non-interactive SSH PATH — `export
PATH=$HOME/.local/bin:$PATH` first). `tools/gnw_probe/screenshot.sh` already
defaults to `PROBE_HOST=rpi-genie5`, so it runs from here unchanged and is the
fastest way to see what the console is actually doing.

Three faults, in the order they were peeled off:

1. **The console was sitting in gnwmanager's on-device stub, not failing to
   boot.** The screen showing `IDLE PROG ERASE` / `ERROR HASH MISMATCH` /
   `FLASH RAM SD` over the OFW clock is **gnwmanager's own UI** — IDLE/PROG/
   ERASE is the operation state, FLASH/RAM/SD the target, the bars a progress
   meter. That screen is not the launcher, has no system list, and does not
   respond like one, which is the whole of "controls did not work" and "32X was
   missing". **Every gnwmanager command leaves the device parked there.** One
   command brings it back: `gnwmanager start bank2`.

   > **This entry originally blamed a stale `/retro-go_update.bin` on the SD
   > root, and that was wrong.** Falsified 2026-08-19: `gnwmanager sdls /`
   > shows no such file on the card, the same screen was on the panel anyway,
   > and it survived a *successful* `sdls` unchanged while its clock kept
   > ticking (00:06 → 00:09) — a live program, not a parked boot. Then
   > `gnwmanager start bank2` brought the launcher straight back
   > (`/tmp/claude-1001/after_start_bank2.png`). The `ERROR HASH MISMATCH` line
   > is **leftover text from some earlier operation**; it is not cleared by a
   > later success, so it says nothing about the command you just ran. No
   > `HASH MISMATCH` string exists anywhere in this repo's own code (grep), and
   > gnwmanager's `firmware.bin` renders text as glyphs, which is why the
   > string does not show up in it either.
   >
   > **Practical rule: finish every device session with `gnwmanager start
   > bank2`, and before reporting a screenshot, say whether it is the launcher
   > or the stub.** Photographing the stub and calling it a dead console is
   > what cost a day here twice.
2. **SWD flash is the working path**:
   `gnwmanager flash 0x08100000 build/gw_retro_go_intflash.bin -- start 0x08100000`
   (INTFLASH_BANK=2). The launcher came up immediately.
3. **Then "손상된 설치가 감지됨" (corrupted installation).** New intflash, old
   cores. `rg_emulators.c:156` shows the launcher parsing each core's header and
   rejecting anything below `EXTERNAL_CORE_HEADER_MIN_VERSION`. `make flash`
   writes intflash **only** — the cores live on the card, and this repo has a
   rule about exactly that. Fix: push all of `sd_content/cores/` with
   `sdpush --dest-path /cores/` (31 files: 29 + `mappers/` contents, which fails
   as a directory and has to be pushed separately).

**State now:** firmware from `testbed-full-20260818-2257` flashed, all cores
pushed, launcher boots, and **Sega 32X is present in the grid** with `/roms/32x`
carrying six ROMs including `둠 (Doom).32x` and `d32xr.32x`.

**Not yet done on the device:** nobody has launched 32X and confirmed input
works in-game, and no screenshot of D32XR running has ever been taken — which is
still what the 41.4 fps figure needs before it can be treated as a target.


---

# The device savestate is now on this machine (2026-08-18, late)

`/home/ubuntu/32x_roms/doom32x_slot0.sav` — 678,917 bytes, md5
`886c10441452f9bae1622b9984016ba8`, pulled off the card with
`gnwmanager sdpull --src-path "/data/32x/둠 (Doom).32x-0.sav"`. It is retail
**Doom**, slot 0. Slot 1 exists too.

Format: the firmware wraps picodrive's stream in an **8-byte header** — magic
`X2XM` then a version word (`md32x_SaveState`, `main_md32x.c`). Skip 8 bytes and
hand the rest to `PicoStateFP(f, 0, read, write, eof, seek)`, exactly as
`md32x_LoadState` does.

> **DONE, 2026-08-19.** The loader is in the rig:
> `RIG_32X_STATE=<file.sav> [RIG_32X_STATE_AT=<frame>] bash
> tools/m7_qemu_rig/run_32x.sh "<the rom the state was taken against>" <frames>`.
> The `.sav` links in as a blob (this runtime has no file I/O), the eight-byte
> header is skipped, and `PicoStateFP` takes the rest. Verified:
> `PicoStateFP=0 consumed=678909/678909 fb=cfbbcd37 nonblank=1`.
>
> **Pair it with the ROM the state was taken against.** Retail Doom was not on
> this machine; it came off `rpi-genie5`
> (`/media/pi/EXTERNAL/miyoo-library/public/roms/thirtytwox/둠 (Doom).32x`,
> md5 `79339867d9d4f58b169753d9a29ea1a5`) and is now at
> `/home/ubuntu/32x_roms/doom_retail.32x`.
>
> **It cost one real bug to get there, and that bug is on the device too.**
> Under `GNW_32X_CORE`, `Pico32xMem->m68k_rom` is a POINTER (the 64K bank image
> lives outside the struct in AHB SRAM), and the savestate wrote it with
> `CHECKED_WRITE_BUFF` — so **four bytes of ADDRESS went into every save file**,
> and the load installed the saving machine's pointer on the loading one. The
> console never noticed because `ahb_malloc` is deterministic and hands back the
> same address; the rig, in a different address space entirely, went silent on
> the very next frame — no fault, no message, just a frame counter that stopped.
> Fixed load-side only (picodrive `c3e4acba`), so the file format and every
> savestate already on a card stay byte-identical.
>
> **The first gameplay profile this rig has ever taken:**
>
> | | host insn/frame | guest SH-2 insn/frame |
> |---|---|---|
> | attract loop (f20, f40) | 10.2M | 69,866 |
> | resumed gameplay (f60) | **39.6M** | **197,674** |
>
> 2.8x the guest work, 3.9x the host work. The attract demo and gameplay are
> not the same program, and now the rig can profile the one that matters.
>
> **The acceptance test, which is the one this file spent pages on: `188 unique
> PCs` is dead.** A 120-frame run with the histogram zeroed at the load frame
> reports **8,329 unique guest PCs over 9,507,376 guest SH-2 insns**, and
> `docs/img_32x_rig_gameplay_f119.png` — rebuilt from `RIG_FB_DUMP` — is a
> first-person Doom frame with the HUD reading AMMO 49 / HEALTH 100% / ARMOR 0%.
> The rig is finally running the program we are trying to make faster.
>
> **And the profile lands where the plan needed it to.** Two inner loops carry
> ~36% of all guest SH-2 instructions:
>
> | loop | share | shape |
> |---|---|---|
> | `0x02049084`–`0x02049094`, 9 insns | **22.95%** | `MOV.B @R3,R8` / `SHLL R8` / `MOV.W @(R0,R8),R8` / `MOV.W R8,@R9` / `DT R12` / `BFS` — fetch texel, shift, colormap lookup, store pixel, decrement, loop |
> | `0x02049284`–`0x0204929c`, 13 insns | **13.12%** | same shape with `SWAP.W`/`AND`/`OR` packing |
>
> That is the texture-mapped column/span inner loop. **The "R_DrawColumn is
> 36.5% of msh2" figure that the HLE plan rests on came from a single device
> profile and had never been reproduced offline — it is reproduced now**, from
> the device's own scene, repeatably, on this machine.
>
> **Then the lever was priced by ablation, and the profile was wrong — in the
> direction nobody expects.** `-DRIG_ABLATE_LOOPS` clamps each loop's DT
> counter to 1 at entry, collapsing it to a single iteration: the loop still
> exits through its own BFS, downstream control flow is unchanged, only the
> pixels are missing. Two arms, same tree, verified different binaries
> (`f7204fc9…` vs `3aa1faf0…`):
>
> | | base | ablated | delta |
> |---|---|---|---|
> | guest SH-2 insn/frame | 165,262 | 105,983 | **−35.9%** |
> | host insn/frame | 15,094,391 | 12,763,684 | **−15.4%** |
>
> Deleting **35.9% of guest instructions removed 15.4% of frame cost.** These
> loops are *cheaper per instruction* than the average guest instruction —
> tight, branchless, straight into SDRAM, with none of the memory-map dispatch
> the average instruction pays, and `gnw_sh2_fastloop` already eating part of
> them. **The ablation ceiling on an R_DrawColumn HLE is x1.18**, not the x1.32
> the instruction share implied nor the x1.22 the table above estimated.
>
> This is the repo's own rule (`rule-price-a-lever-by-ablation-not-by-profile`)
> firing in the *opposite* direction from usual: the standard failure is a
> profile that under-counts a lever because memory stalls are invisible in an
> instruction count. Here an instruction count **over**-counted it.
>
> ⚠️ And the mirror of that caveat applies to this very number: **the rig counts
> host instructions, not device cycles.** QEMU has no caches and no wait states,
> so a memory-bound loop is exactly the shape the rig under-prices. The device's
> share could be higher than 15.4%. A device A/B is the arbiter; x1.18 is the
> rig's floor on the ceiling, not the answer.
>
> ⚠️ **Do not quote an fps from this run.** Per-frame host cost over the
> gameplay window is wildly dispersed — `avg 1.06G, min 8.0M, max 3.74G
> insn/frame` — so the mean is not a speed. The guest-instruction histogram is
> unaffected (it counts guest insns, not host time), which is why the profile
> above stands while the frame-cost number does not. Explaining that spread is
> the next job: the cheap frames and the 3.7G ones differ by 460x, and at f100
> the guest did only 59,391 insns while the host burned 85M — host work with no
> guest progress, i.e. the scheduler, not the renderer.

**Load it in the RIG, not in tools/pico_host.** The host driver is a 64-bit
build and the state stream carries the device's pointer widths; it stops at

    unexpected len 4, wanted 8 (sizeof(Pico32xMem->m68k_rom))

because `m68k_rom` is a pointer — 4 bytes on the device, 8 on the host. The QEMU
rig is 32-bit ARM and matches the device exactly, so the same eight-byte skip
plus `PicoStateFP` should load there without any of this. That is the last piece
between us and profiling the *device's own scene* offline, forever, instead of
inheriting one figure from one device run.

# Device measurement, same evening: ~23 fps

The user reports Doom at **~23 fps** after this deployment, against the 17.02
gameplay anchor. Not yet confirmed by our own instrument, and worth confirming,
because the most likely explanation is uncomfortable: **the card's cores were
stale**. `make flash` writes intflash only, so a device flashed that way keeps
running whatever core `.bin` the card already had — and every core-side change
since that card was last written (the OC-level-2 default among them) would have
been sitting unused. If that is what happened, some earlier device numbers were
taken against a core nobody meant to measure.
