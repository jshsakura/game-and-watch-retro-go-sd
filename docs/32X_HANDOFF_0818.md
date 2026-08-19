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
> **The device has now answered it: a guest data read costs 29.2 cycles**
> (msh2, DWT-bracketed, 1-in-29 sampling, n=141,684, on the savestate-resumed
> gameplay anchor; ssh2 41.7; a July measurement of 29.3 corroborates). That is
> **2.8x an opcode fetch's 10.3**, and it is exactly the axis the rig cannot
> see — QEMU has no wait states, so every rig A/B here under-prices
> memory-touching work.
>
> **The per-region split is measured now, and it CLOSES the axis.** Bucketing
> `gnw_probe_rd` by `(a & 0xdf000000)`, same anchor:
>
> | region | cycles/read | n (msh2) |
> |---|---|---|
> | cart ROM (XIP flash) | **43.3** | 31,658 |
> | SDRAM (internal RAM) | **22.0** | 40,181 |
> | 32X DRAM | 32.5 | 2,094 |
> | other (peripheral/regs) | **106.5** | 125 |
>
> ssh2 pays more: rom 51.0, sdram 23.8.
>
> My inference of 51-61 for ROM was wrong because I assumed SDRAM cost 8-15.
> It costs **22.0** -- the code's own warning about "a 256 KB SDRAM working set
> against a 16 KB D-cache" priced, at last.
>
> And that is what kills the lever. A RAM cache lives in RAM_EMU, so a cache
> **hit also costs 22.0**. The saving is 43.3 - 22.0 = **21.3, less than half**
> the flash cost, not the full 43.3:
>
> | | |
> |---|---|
> | 19,785 cached reads/frame x 21.3 | 421 K cycles = **2.1%** |
> | minus a hit test on all 28,926 ROM reads (3-5 cyc) | -0.4 to -0.7% |
> | **net** | **~1.5%** |
>
> Eight kilobytes of RAM_EMU and a test on every guest read, for 1.5% -- and
> `rule-a-test-that-skips-work-loses-on-this-chip` is about exactly that test.
> **Closed.** The measurement did not open this axis; it closed it, which is
> what it was for.
>
> Two things worth carrying from those numbers: peripheral/register reads cost
> **106.5** cycles, so a game that polls registers hard has *that* as its wall
> rather than anything here; and the aggregate 29.2 against the bucket-weighted
> 31.5 differ by 8% (different windows, different `MD32X_PROFILE_FRAMES`), so
> say which one you are quoting.
>
> The AHB assert that blocked this was diagnosed too, and it was neither the
> save nor the interpreter work: the profile build's own delta pools at
> `MD32X_PROFILE_FRAMES=64` exhausted the AHB pool during the savestate load
> (`get_bios`'s 64 KB bank image allocates from it too). `FRAMES=64 -> 32`
> gives 1.5 KB back and it boots. The `_Static_assert` guarding that pool
> checks the **static** sum only; the load path allocates at runtime, so the
> assert passes and the device still dies.
>
> ⚠️ And the mirror of that caveat applies to this very number: **the rig counts
> host instructions, not device cycles.** QEMU has no caches and no wait states,
> so a memory-bound loop is exactly the shape the rig under-prices. The device's
> share could be higher than 15.4%. A device A/B is the arbiter; x1.18 is the
> rig's floor on the ceiling, not the answer.
>
> **The 68% nobody had opened.** `msh2 (interp+bus)` is one bucket in the phase
> table and it is two thirds of the frame; this is what is inside it. Counted
> from the disassembly of `sh2_execute_interpreter` in the rig build, per
> dispatched guest instruction, before the operation's own body runs:
>
> | prologue/epilogue | host insn | what |
> |---|---|---|
> | `0x28`–`0x50` | ~11 | fetch: pc load, SDRAM region test, ROM region test, mask load+test, `p_rom` load, `ldrh` |
> | `0x52`–`0x84` | ~8 | fastloop opcode pre-filter (`(op & 0xf980) == 0x8980`, `op == 0xaffe`) |
> | `0x58`/`0x76` | ~4 | `ldrd`/`strd` 64-bit counter — **`RIG_SH2_COUNT`, rig-only, not in the device build** |
> | `0x5e`–`0x72` | ~4 | `pc += 2`, `ppc = pc`, `delay = 0` |
> | `0x88`–`0x90` | ~3 | `tbh` dispatch |
> | `0xbc`–`0xfe` | ~8 | cycle decrement, IRQ-pending check, loop back |
>
> **~38 host instructions of fixed overhead against 70.5 total, so the average
> operation body is only ~30.** The interpreter spends more on bookkeeping than
> on the instruction. That is the shape of the dominant cost, and no amount of
> making the *operations* faster touches it.
>
> Two things fall straight out. First, the fastloop pre-filter runs on every
> dispatched instruction to catch opcodes that all live in two nibbles (`0x8xxx`
> and `0xaxxx`), and gcc rebuilds both `movw` constants every iteration instead
> of hoisting them. Second — a methodological one — **`RIG_SH2_COUNT` puts a
> 64-bit load-add-store on every guest instruction in the rig.** A/B deltas are
> unaffected (both arms carry it), but 70.5 host-insn/guest-insn is the *rig's*
> ratio, not the device's, and must not be quoted as the device's.
>
> **Two landed off that anatomy, both hash-gated, both in the path every 32X
> game runs through:**
>
> | change | host insn/frame | delta |
> |---|---|---|
> | baseline | 15,094,391 | — |
> | + cart-ROM data fast path (`202e4a29`) | 14,786,244 | −2.04% |
> | + fastloop nibble gate (`f9e2bfb8`) | 14,582,403 | −1.38% |
> | + opcode fetch window (`ae185661`) | 13,630,378 | **−6.53%** |
> | + inline data-read fast paths (`56794fe1`) | 13,410,684 | −1.61% |
> | − restore the `!delay` IRQ guard (`494e5998`) | 13,560,320 | +1.12% |
> | **+ texture-column HLE (`9528be13`)** | 12,560,257 | **−7.37%** |
> | **+ span HLE (`b5c9a6a6`)** | 11,318,516 | **−9.88%** |
> | **compounded** | | **−25.02%, x1.334** |
>
> **Both of Doom's renderer inner loops now run natively**, and the second one
> is the larger win. Framebuffer and audio hashes unmoved throughout.
>
> **Bespoke beat general, and that was measured, not preferred.** A general
> folder was built first — a whitelist of foldable ops, exactly one `DT`,
> nothing writing T after it. It identified both loops correctly and still came
> out **slower than the column loop alone** (12,797,823 against 12,560,257),
> because a per-op dispatch inside the fold costs more than the
> per-instruction machinery it removes. Zero dispatch is the whole point.
>
> The general version also moved the audio hash (`af8c118b` → `637abf77`) with
> the framebuffer identical — and **the cause was not the folding.** It wrote
> the reject slot and returned on any shape it did not match, starving the
> SDRAM-poll and countdown fast paths below it of loops they would have
> handled, and those consume icount in bulk. The tell was that the guest
> instruction count was *identical* across both arms: no extra loop was being
> folded at all. A bespoke matcher cannot make that mistake — a shape it does
> not recognise simply falls through.
>
> ⚠️ The two HLEs together (−16.53%) slightly exceed the −15.4% ablation
> ceiling measured earlier. That is not an HLE beating "remove the work
> entirely": the ablation arm carried its own per-instruction probe hook, which
> inflated it. **An ablation ceiling measured with an always-on hook is a lower
> bound, not a ceiling.**
>
> **The R_DrawColumn HLE is built.** Not the ablation — the real thing: the
> seven-instruction inner loop runs natively as a new `gnw_sh2_fastloop`
> pattern, matched structurally (eight opcode forms plus register agreement,
> register numbers read out of the matched opcodes, never an address), so it
> fits any build of this renderer rather than one cart.
>
> −7.37% against the −15.4% ablation ceiling: the HLE still does the real
> memory work, and that is the half it cannot remove. **On the device that half
> costs more than it does here** — 43.3 cycles a cart-ROM read, 32.5 a DRAM
> write, against a rig with no wait states — so the device split will differ.
> Measure it; do not infer it from the rig number.
>
> Two things that had to be got right rather than assumed:
> - `BFS` assigns `sh2->delay` **unconditionally**, so the delay slot runs
>   whether or not the branch is taken. A folded iteration is therefore
>   *[previous delay slot] + [one body]*, which leaves exactly the state the
>   fastloop was entered with.
> - The body **calls the interpreter's own op functions** (`sh2.c` is included
>   above that point, so `ADD`/`MOVBL`/`ADDC`/`SHLL`/`MOVWL0`/`DT`/`MOVWS` are
>   in scope). The first draft reimplemented them and got `ADDC`'s carry wrong
>   for `b=0xFFFFFFFF` with carry-in 1 — and reimplementing would also have had
>   to reproduce `SHLL` writing T, `DT` setting `no_polling`, and every op's
>   `sh2->ea`. Calling them is the difference between *should be* identical and
>   *is*.
>
> **That last row is a correctness payment, and it was worth it.** The
> `&& !sh2->delay` on `sh2_execute_interpreter`'s IRQ check had been dropped
> for "never happens in 900 attract frames", and it rode into `af24e213` as an
> unrelated working-tree change — a hazard of several sessions sharing one
> worktree, and a reason to read `git diff --cached` before committing rather
> than staging a whole file.
>
> The guard is load-bearing and the mechanism is exact, not statistical:
> `sh2_do_irq` pushes `sh2->pc` and overwrites it with the vector, and neither
> reads nor clears `sh2->delay`. Service an IRQ right after a delayed branch —
> `delay` = slot address, `pc` = branch target — and the next iteration still
> sees `delay` set, runs the delay-slot instruction, then applies its `pc -= 2`
> to what is now the **interrupt handler's entry**. The handler starts two
> bytes short. Rare, silent, state-corrupting.
>
> The tell was in the file itself: `sh2_execute_interpreter_trace` never lost
> the guard, so the two interpreters disagreed. And "attract did not reproduce
> it" is not evidence of safety on a core where attract and gameplay are
> 188 unique guest PCs against 8,586.
>
> **And they made the interpreter smaller, which on this device is a second
> win.** Interpreter `.text` on a device-shaped build: 10,897 bytes this
> morning, 8,241 with the nibble gate and fetch window, 10,825 with the read
> inlining on top — still below where it started. That matters more than a
> size table usually does: the linker script records a **device-measured +30%
> fps from moving this interpreter into ITCM at an unchanged instruction
> count**, pure memory-stall removal that QEMU cannot see. Instruction counts
> systematically under-tell the story here, in the direction of these levers.
>
> **Three axes closed by the compiled output, all in one hour, all sound
> reasoning:**
>
> | tried | result |
> |---|---|
> | region test ROM-first | 12 → 15 instructions; gcc hands r6 to the SDRAM mask |
> | inline `RL` only | 13,989,652 — **worse than no inlining at all**, +2.6% |
> | (kept) inline all three read macros | −1.61% |
>
> A partial inline disturbs register allocation across the op handlers without
> the compensating wins. There is no "inline the biggest one first" here.
>
> Framebuffer checksums (`1dca767c`/`e46856ae`), audio hash (`af8c118b`) and
> guest instruction count (165,262) identical across all three; arms md5-verified
> different every time. `GNW_NO_ROM_DATA_FASTPATH` and
> `GNW_NO_FASTLOOP_NIBBLE_GATE` turn them off.
>
> The fetch block is done: 17 instructions to 13 on a device-shaped build, by
> folding the region tag, mask and base into one object so the "is the fast path
> legal" test disappears into the tag compare. **Still unopened: the
> ~8-instruction cycle-decrement/IRQ-check epilogue** — `sh2->icount` is
> decremented and stored back to memory on every single instruction, and
> `sh2->test_irq` is loaded on every single instruction.
>
> **Two method rules came out of this, both earned the hard way in one hour:**
>
> - **Count the compiled output on a DEVICE-shaped build before running any
>   A/B.** `RIG_SH2_COUNT` does not merely add four instructions; it holds a
>   register, and gcc generates materially different code with it gone. The
>   nibble gate is eight instructions in the rig and three on the device,
>   because the device build has a register free to hoist its constant into.
>   Every rig A/B here therefore *under*-prices the lever it measures.
> - **An ablation is verified by what the compiler emitted, not by what it was
>   called.** Reordering the region tests to put ROM first is correct reasoning
>   and a regression: gcc hands r6 to the SDRAM mask and reloads the ROM mask's
>   address on the ROM path, 12 instructions to 15. Closed; do not retry.
>
> **Closed, and not the way it looked: the packed-pixel run detector survives.**
> Its `+22% without it` came from a profile taken before the gameplay anchor
> existed, so it was a candidate for the same "measured on attract, shipped for
> gameplay" fault that has cost this project three times. Re-run on the
> gameplay anchor with `GNW_PP_NO_RUNDET`, hashes identical both arms:
>
> | | host insn/frame |
> |---|---|
> | detector on | 13,560,320 |
> | detector off | 13,467,551 (−0.68%) |
>
> So on textured gameplay it *is* a net loss — the predicted sign. The
> magnitudes are what decide it: it costs **0.68% when it cannot help** and
> saves **22% when it can**. Keep it. Trading Doom's 0.68% against 22% on the
> flat-content 32X games (Virtua Racing, Star Wars Arcade) is a bad bet, and a
> per-game switch is the class of hack this repo forbids. Do not re-run this
> A/B; the answer is recorded.
>
> The write path is closed too, on arithmetic rather than an A/B: the census's
> totals are 49-frame cumulative, so writes are **14,314 per frame, of which
> 6,742 are SDRAM** — inlining them the way the reads were inlined is worth at
> most 0.4%, which does not pay for the ITCM. Read the census per frame, not
> per run.
>
> **An anchor the user can overwrite by playing is not an anchor.**
> The device measurement of a guest data read (below) was taken against the
> savestate sitting on the card. The user played that evening, the save was
> overwritten, and the *same v26 binary reflashed the next day* died 63 frames
> into boot on an AHB-pool assert. Nothing about the firmware changed; the
> anchor did. Yesterday's number stopped being reproducible.
>
> The fix is to treat the anchor as an artifact: the pinned copy is
> `/home/ubuntu/32x_roms/doom32x_slot0.sav`, 678,917 bytes, md5
> `886c10441452f9bae1622b9984016ba8` — the file every rig measurement in this
> document was taken against. Push it to the card before measuring, and use
> **slot 1** so the user's own save in slot 0 is never touched.
>
> ## The device settles it: 17.95 → 21.39 fps, +19.2%
>
> Paired A/B on hardware, one tree (HEAD), one define apart, 1800 frames ×3 per
> arm on the savestate gameplay anchor in **slot 1** (the user's slot 0 never
> touched):
>
> | arm | drawn fps | samples |
> |---|---|---|
> | levers off | **17.95** | 17.95 / 17.96 / 17.95 (spread 0.01) |
> | levers on | **21.39** | 21.39 / 21.38 / 21.38 (spread 0.01) |
>
> `32x.bin` md5 `6bba1b4f` (41,009 B) against `bdf42451` (45,129 B) — two
> programs, verified, and the cores were pushed with the intflash rather than
> without, which is the mistake that made yesterday's numbers meaningless.
>
> **The rig over-predicted: x1.334 against the device's x1.192.** That is the
> expected direction and the reason is now measured rather than argued — an
> HLE removes interpreter work and *keeps* the memory work, the rig prices
> memory at zero (no wait states), and the device charges 43.3 cycles for a
> cart-ROM read. Both renderer loops read ROM twice per pixel.
>
> **So the rig's bias has a sign that depends on the lever.** It *under*-prices
> anything that shrinks code or improves ITCM residency (the linker script
> records +30% fps from an ITCM move at unchanged instruction count), and it
> *over*-prices an HLE that leaves the memory traffic in place. Both directions
> were confirmed in one day. Quote rig numbers as rig numbers.
>
> ⚠️ **Do not quote an fps from a rig run.** Per-frame host cost over the
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


---

# Native DOOM: the veneer wall has a one-flag answer, and it was never tried

The nhdoom port was shelved 2026-06-28 on one fault. The engine reached
`R_InitTextures` — boot, WAD, flash-cache relocate, `R_Init`, `R_InitData` all
passed — and died in `FindLumpByName` at its `bl __memcpy_veneer`:

    __memcpy_veneer:  ldr.w pc, [pc] ; .word 0x08100611

Executing that **from OSPI XIP flash**, the self-referential load-into-PC reads
0 on the D-side. The literal was proven correct — printed identical before and
after a full `SCB_CleanInvalidateDCache` — and plain `ldr rX,[pc]` data literals
from XIP work fine, which the engine demonstrated by running on them. Only the
`ldr pc` branch hazards.

The veneer exists because `.doom_xip` sits at the `0xD00D0000` sentinel and the
firmware at `0x08100000`: 3.35 GB apart, so every cross-boundary `bl` is out of
±16 MB range and the linker synthesises one. `memcpy` lives in firmware libc,
so a struct copy in engine code reaches it through a veneer.

**`-mlong-calls` removes the veneer rather than fixing it.** Verified by
codegen, `arm-none-eabi-gcc -mcpu=cortex-m7`:

| flags | emitted call |
|---|---|
| none | `bl <memcpy>` → linker veneer → `ldr.w pc,[pc]` |
| `-mlong-calls` | `ldr r3,[pc,#4]` then `blx r3` |

A data literal load — the form that works from XIP — followed by a register
branch. No PC-relative load into PC anywhere, and **no linker veneer is
generated at all**, so there is nothing left to hazard. This is exactly the
`ldr r12,[pc]; bx r12` shape the June note listed as untried, applied by the
compiler to every call instead of by hand to one veneer.

It also fits the relocation machinery already in place. The literal holds the
callee's absolute address: XIP→firmware literals are real `0x08xxxxxx` addresses
needing no patch, and XIP→XIP literals are `0xD00D…` sentinels, which is
precisely what `PatchDoomRegion` scans for and patches. Every cross-reference
becomes a plain data word — more uniform than the mix of veneers and literals
it replaces.

Cost is one instruction and four bytes of literal per call. The engine was
written for a 64 MHz nRF52840; this device is a 340 MHz M7.

**Status: untested on hardware.** What is established is that the flag produces
the instruction form the June evidence says survives XIP, and that it removes
the construct that failed. Apply it to the `build/nhdoom/*.o` and `build/doom/*.o`
recipes, count `__*_veneer` in the link map (the target is zero), then flash.
