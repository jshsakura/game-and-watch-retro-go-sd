# Sega 32X — where this stands, and what to aim at next

Written 2026-08-15, at the end of a day that produced one large win, one large
correction, and one axis closed by measurement. Read `32X_CLOSED.md` §0 and §0b
first; this file is the working state and the queue, not the ledger.

Everything below is measured on hardware unless it says otherwise.

---

# NEXT SESSION — start here

**Read [issue #45](https://github.com/jshsakura/game-and-watch-retro-go-sd/issues/45)
first.** It is the public write-up of where Doom-on-32X stands, every axis closed by
measurement, and why the two big ones are blocked. This file is the working detail
behind it.

## Build on ubuntu-lab, not on the session VM

A 32X arm takes **~12 minutes** on the 4-core VM the sessions run on and **91
seconds** on `ubuntu-lab` (20 cores, 123 GB). Serial builds on the VM were the real
bottleneck for most of 2026-08-21.

    ssh ubuntu-lab '~/gnw_build.sh <name> <make vars...>'      # ~/gnw, artefacts in ~/gnw_arms/<name>
    scp -r ubuntu-lab:gnw_arms/<name> /tmp/gnw_arms/           # then flash from here
    bash tools/gnw_probe/arm32x.sh flash <name>

`~/gnw` is checked out at the tip; `git fetch && git checkout <sha> && git submodule
update --init --recursive -j8` before a run. For parallel arms give each its own
checkout (`DIR=~/gnw2`) — `build/` is shared, and `--name` collisions are docker
error 125.

**The console stays with one owner.** Builds fan out; flashing and benching do not.

## 2026-08-26f: the IWDG is withdrawn -- a watchdog that cannot sleep

The independent watchdog was armed at boot (9d98ad04) to cover the one
observed runaway that wipes WWDG1->CR on its way through the peripheral
space. It never ships. A measured STOP2 run proved the cost: with
FZ_IWDG_STOP showing set in OPTSR_CUR (0x1006aaf0, bit 17), the game
sleep was still bitten at the ~33 s mark -- DR10 = IWDGRSTF|CDRSTF|
PINRSTF is the hardware's own signature. Either the option byte does not
mean freeze on this part (RM0455's polarity was not re-verified against
silicon; zero devices were option-programmed, and that stays forbidden),
or a software-activated IWDG is out of its reach. Reading the bit and
declaring safety was the tenth-class mistake this campaign keeps
finding: an option byte is only proven by the reset it does not cause.
6effe430 reverts the arming; WWDG stays (windowed, verified-to-arm,
silent in STOP2).

Two contaminated verdicts are retired with it. The b160 "STANDBY soak
passed" used emu>100 as success -- a criterion that cannot tell an alarm
wake from an IWDG reset, and the run predated RSR discipline. Rule: a
sleep verdict always reads RSR; "alive" is not "woke". And the wild-5
reproduction (live load_state under gdb) is a deterministic runaway
generator, NOT a shipped trigger -- the audit behind fbfa50e6 walked
every shipped load path (boot resume before the loop, B-button at a
frame boundary, pause menu on a stopped machine) and found none that
load mid-frame. The wild-4 trigger remains unknown; what the guard buys
is defense in depth, not a cause.

Open, unmeasured, recorded only: whether the alarm path added to
SleepModeEnterAndResume actually wakes from STOP2 on its own (the
harness run had the IWDG bite first). EXTI rising is enabled in code
(rtc.c HAL_RTC_SetAlarm_IT); the live test runs when the device frees
up, before the gw_sleep.c commit lands.

## 2026-08-26c: the XN firewall lands -- the silent runaway gets a name

Three runaways (two stretcher-era freezes, one on shipping-behaviour
wd15) all landed the same way: PC inside guest 68K RAM (PicoMem
0x240db098+, addresses 0x240e90xx-91xx), fault registers zero, watchdog
silent, counters frozen. Guest code bytes occasionally decode as valid
Thumb, so the CPU can wander there indefinitely.

Region 7 (the last free MPU region) is now an execute-never firewall at
0x24080000-0x24100000 (512KB), installed at app_main_md32x entry and
cleared by the reset that follows every core exit. TEX=001/C=1/B=1
mirrors the background WB/write-allocate policy exactly -- benched
against its base bracket: 34.373 vs 34.493 = -0.35%, noise (had the
first-draft TEX=000 shipped, write-allocate loss on 512KB of
write-heavy guest RAM would have been measurable).

The gate that proved it: injected jump to 0x240e9110 mid-game ->
"current mode: Handler MemManage", DR3=1 (fault crumb), DR4=0x240e9110
(exact PC), CFSR=IACCVIOL. First attempt failed because that session
was the first boot after an SWD flash and region 7 was already gone --
clean boot reproduces perfectly; recorded as a flash-adjacent artifact.

Observation, not a hypothesis -- three cases now (2026-08-26): the first
boot after an SWD flash has behaved differently from a normal boot in
three different subsystems -- the three boot hangs (all first boots
after flashing), the one wild watchdog-not-armed event (suspected same
family), and region 7 missing above. Cause unknown; no build will be
burned chasing it. The rule that follows the evidence: treat anything
observed on the first boot after an SWD flash as non-evidence --
power-cycle first (tools/gnw_probe/powercycle.sh), then bench or
diagnose.

Honest limits, so nobody misreads a future silence:
  - 185KB of bss (0x24052a08-0x24080000: m68k state + the head of the
    32X SDRAM mirror) is NOT covered. A runaway into that range still
    dies silently -- crumb silence does NOT mean the firewall failed.
  - The firewall names the crash; it does not fix it. Whatever corrupts
    a return address into guest RAM is still open (see #46). But the
    next occurrence now leaves PC/LR/CFSR instead of nothing.

## 2026-08-26: the release ships 44100 -- 22050 without the stretcher is the sound the user already complained about

The user verdict came back: 30 fps is monumental, the audio is still awkward
but acceptable -- heard on aud3, which is 22050 + stretcher. The field
reports, read together, decide the release:

    triple (22050, no stretcher): "some crackling"  <-- 22050 ALONE
    aud3   (22050 + stretcher):   "awkward, but I call it the limit"

So 22050 without the stretcher is the sound the user already filed a
complaint about, and the stretcher is parked in audring behind an
unresolved intermittent freeze. +2.00% is not worth shipping a reported
audio regression; the headline barely moves (34.0 -> 33.3 fps, still
+24% over base0 and still over 30).

Decision (revert of 99e4cd02): MD32X_AUDIO_RATE_OVERRIDE stays a knob,
default 0 = 44100. FOR THE NEXT PERSON: when audring's freeze is solved,
raise 22050 AND the stretcher TOGETHER in one commit. Never 22050 alone --
that build is the crackle the user rejected.


### The DRC memory question, closed the same day it was asked

Before anyone writes a Thumb-2 dynarec backend, the code cache needs a home:
executable RAM. Measured live on the device during DOOM (2026-08-26):

| Region | Free (contiguous) | Executable? | Speed (measured) |
|---|---|---|---|
| RAM_EMU 0x2404b000 | 108 B | yes (it is the overlay) | load 3.0 cyc (cached, D1 AXI) |
| AHB SRAM 0x30000000 | 3,928 B | **FAULTS on live exec** (2/2 runs; MPU XN suspected) | data load 1.0 cyc |
| DTCM heap 0x20003ea0 | **56,268 B** | **yes — executes at ITCM speed** | exec 1.50 cyc/iter == ITCM's 1.50 |
| ITCM | ~3.1 KB (taken) | yes | exec 1.50 cyc/iter (reference) |

Exec benchmark: 100k-iteration 3-insn loop self-sampling DWT CYCCNT; ITCM
150,025 vs DTCM 150,027 cycles -- identical. Loads cached: AHB 1.0,
internal flash 2.0, RAM_EMU 3.0 cyc. (XIP micro-benchmark faulted twice;
the field number stands: ~10.4 cyc/fetch, 30% of the frame.)

Draw2FB (83,976 B of the AHB pool) is LIVE -- verified by content changing
between reads during play, and main_md32x.c documents the NULL-stub as a
wild-read crash. RAM_EMU has no dead structs to reclaim: every large symbol
(gnw_32xmem 532K, PicoMem 140K) is hardware-fixed guest memory.

Verdict: the largest contiguous executable RAM is DTCM at 55 KB -- under the
64 KB bar. The SH-2 dynarec axis closes for lack of a code cache, before the
Thumb-2 backend question even starts. (DRC would also need its drcblk/drclit
arrays in RAM_EMU, which has 108 B.) DTCM's 55 KB remains the right place for
data levers, tl2-style.

## 2026-08-26e: no PMU on this silicon, and one guest read costs 13.8 cycles

Two facts for whoever tries to profile this core next.

**Do not expect a PMU just because it is a Cortex-M7.** This chip's DWT
implements CYCCNT and nothing else: CTRL reads 0x40000001 (NUMCOMP=4,
NOCYCCNT=0 -- the event counters claim to exist), but CPICNT/EXCCNT/
SLEEPCNT/LSUCNT stay at zero through five seconds of full-rate DOOM with
TRCENA set, a disable/re-enable cycle, and direct writes that read back.
The PMU at 0xE0009000 is absent (all zeros, no implementer field) and the
vendor CMSIS core_cm7.h ships no PMU definitions. The only hardware cycle
instrument on this device is CYCCNT -- window it around the region you care
about, or ablate. (Measured 2026-08-26 trying to answer "what is the SH-2
interpreter bound by"; the window measurement itself works fine: the SH-2
slice owns 75.4% of frame cycles, 8.86M of 11.76M at 340 MHz, vs 67.5% by
host instructions in the rig -- cycles weigh it heavier.)

**One guest memory read costs ~13.8 cycles of frame time.** With no LSU
counter to ask, the price was measured by duplication: a build that performs
every ROM/SDRAM fast-path guest read twice (same address, result discarded
into a volatile sink) must be bit-identical -- and was: rig sound hash and
framebuffer checksums unchanged, so the workload provably did not move. The
sandwich: base 34.20 fps, duplicated 30.52, base 34.20 again (zero drift).
The added 3.53 ms per frame over 87,086 reads/frame is 40.5 ns = 13.8
cycles per read at 340 MHz -- and that is the *marginal* cost of
re-executing the region test, the load, and the sink store; the original
read's own cost is below it. Guest reads therefore own, at most, ~12% of
the frame (16% of the SH-2 window): real, but not the dominator. The rest
of the window is the interpretation itself. The A/B knobs never left the
bench tree (MD32X_DBL_RD in the Makefile of that day's scratch tree); the
patcher that made them is preserved in the session notes if the
different-line variant (cache-miss share) is ever wanted -- note the ITC
budget choked on the inline hook the first time, so a retry needs a
noinline helper.

Also closed the same day: lfo_pm_table (131 KB of XIP) is *not* the next
ym_tl_tab. update_lfo_phase runs once per channel per render call (8 call
sites, all in chan_render's head precalc -- never inside the sample loop),
measured 27.2 entries/frame on DOOM, 128 B of working set, 0.05% of the
frame at worst-case miss cost. Access pattern, not table size, decides
which table is a lever.

## 2026-08-25d: 32X performance lands -- every axis closed, three items open

The skeleton-ceiling chain finished the SH-2 axis, and with it the whole
performance map for this core:

  v1 (control flow removed) = runaway, 0 frames -- the PC landed in guest ROM
     XIP (0x93f1480e): fetch "succeeds", no fault, garbage execution.
  v2 (fastloop gates also removed) = 16.10 fps, -54.6% -- control flow lived,
     but the BT/BF poll bundle was gone (0x8 is 20.78% of msh2 dispatches;
     its 76-insn per-opcode cost was the fastloop doing real work).
  v3 (honest ceiling: control flow verbatim, bookkeeping only removed --
     unconditional ppc store, illegal check) = 34.770 vs bracket 35.200,
     -1.22%. --> the +10% bar was missed: the SH-2 C/Thumb-2 lever axis is
     CLOSED. Static insn counts do not convert into device time here.

  Placement caveat, recorded with the number: v3 changes code layout (dense
  16-way dispatch), and this core has an unexplained -11.6% from layout alone
  (2026-08-22). -1.22% is inside the placement-noise band. The conclusion
  survives anyway: a ceiling measurement that actually removed work and came
  out negative cannot have +20% hiding behind it.

RULE, learned twice in one day: suspect the measuring tool first.
  (1) RIG_SH2_COUNT rides every rig build unconditionally -- it inflated the
      "fixed cost" to 48-51/60.9; the device-static truth is 23 skeleton +
      ~10 handler ~= 33.
  (2) The power-cycle hook hardcoded the emu counter at 0x20002404; the
      crumb wiring moved it to 0x2000242C, so every check read dead BSS and
      the hook reported INCOMPLETE on every flash while the power cycles were
      succeeding. The fix resolves the symbol from the flashed ELF
      (tools/gnw_probe/powercycle.sh). A gate that fails silently gets
      ignored; a gate that lies gets believed.

Final map (all measured on hardware, DOOM gameplay anchor):
  LANDED: tl2 ym_tl_tab DTCM half-tables +21.5%; Cz80_Exec ITCM +3.5%;
  OC2 340/113 +3.73% (30-min soak, NOW THE DEFAULT -- GNW_OC2_PLLQ ?= 6);
  ar22k 22050 Hz +2.00% (listening verdict pending, stays opt-in).
  CLOSED, do not retry: lfo_pm_table (0.06%), musashi jump table (2% bucket,
  SH2-heavy ROM), ITCM for anything under the 16K I-cache (fmpath rule),
  write-bus fast paths (3 arms, all flat-to-negative), ppc conditional store
  (+0.023% wash), GBR 0xC5 (100% 32X register I/O -- real work), Z80 HLE
  (music is a self-running sequencer; ceiling 37.13 needs it gone entirely),
  icount local cache (pico_int.h observers), skeleton C pass (this section),
  clock ladder (312 < 340 > 348 > 353), 16 kHz audio (wash vs 22050),
  DTCM HighLnSpr/chan_render (layout regressions), draw/VDP and LTDC L8
  scanout (fmpath rule / CLUT full), stretcher defaults OFF (equal fps,
  simpler path -- listening decision to re-enable).
  Cumulative: base0 26.81 -> 34-35 fps (+27-31%).

OPEN (3):
  1. The "sporadic crash" (ratio 1.5160 run, fault regs 0) -- CAPTURED and
     NAMED 2026-08-26: it is the pause banner, not a crash. emu frozen,
     drawn repainting at vsync (62.7 fps), watchdog refreshed inside the
     wait loop -- every field of the signature matches. Bounded in code:
     the banner's while(1) asks odroid_idle_timeout_expired() and leaves
     via _save_state_and_sleep (odroid_overlay.c:536-540), so the SHIPPED
     RELEASE IS NOT EXPOSED; the 57-minute 07:16 event was the bench build
     (GNW_NO_IDLE_OFF=1) with nobody to press the button. Alarm, sleep and
     low-battery are each ruled out in code (daily alarm rolls to next day
     at boot; the ring self-clears after 60 s and repaints at ~16.7 fps;
     no low-battery modal exists). One bit decides what is left: DR13, the
     gamepad bitmask the entry poll saw. 0 = software defect (raise
     priority); VOL bit set = pin glitch or finger, hardware/ops, not a
     release concern. Modal crumbs DR12-14 are deployed (1ca4b59e) and a
     long-run monitor is watching for the next occurrence.
  2. Hook SIGPIPE -- the inline hook dies of exit 141 in the tool
     environment; it is now a standalone verified script instead:
     tools/gnw_probe/powercycle.sh <elf>. Bench procedure: after
     arm32x.sh flash, run it manually (alarm -> STANDBY -> wake -> cold
     boot, end-to-end verified 2026-08-25).
  3. Audio listening verdict at 22050 (user's ears; MD32X_AUDIO_RATE_OVERRIDE
     until then).

## 2026-08-25c: the SH-2 icount axis closes, and the skeleton ceiling turns out to be negative

The icount C-pass died on evidence, not effort. `pico_int.h` (non-DRC block) defines
`sh2_cycles_left(sh2)` and `sh2_burn_cycles(sh2, n)` as direct `->icount` accesses, and
they are used **mid-slice** by the 32X layer: `pico/32x/32x.c:669/762` read the live
budget for poll/event timing and `pico/32x/memory.c:1695/1777` burn cycles on DREQ FIFO
stalls. A loop-local `icnt` leaves `sh2->icount` stale inside the running slice, so poll
timing drifts (+1% uniform divergence from frame 20, boot onward) and burns vanish --
provably non-equivalent (rig snd hash mismatch, fb checksum mismatch). The rig gate
caught it: base+noFL reproduced the baseline hashes exactly, isolating the divergence to
the sh2.c transformation. The 37-site macro refactor would hit the same wall. Axis closed.
A secondary asset: the fastloop boundary is provably transparent -- patched+no-fastloop
and base+no-fastloop are hash-identical, so fastloop publish/reload at the single call
gate is correct as built.

The skeleton-ceiling ablation (leader-directed, wrong-emulation-allowed, fps only)
came back **negative**, and the reason matters more than the number:

- skelceil v1 (delay arm, IRQ tail, fastloop gates, ppc all removed): zero frames.
  Post-mortem: PC=0x93f1480e, LR=0x93f389a9 -- the interpreter ran guest ROM XIP bytes
  as ARM code. QSPI fetch "succeeds", so no fault fires (DR3=0). Runaway. delay + IRQ
  delivery are load-bearing for control flow.
- skelceil2 (delay arm + IRQ tail restored verbatim; ppc store, direct tracking,
  fastloop gates, illegal check still removed): **16.10 fps vs base0c bracket 35.44 =
  -54.6%**. Game runs (ratio 1.0000, emu=drawn), so this is not a hang -- it is the
  cost of removing the fastloop gates. 0x8 BT/BF is 20.78% of msh2 dispatch and the
  opcost census put ~76 host insn around the 0x8 group: that is the fastloop bundle's
  real work. Strip the gate and every BT/BF poll loop pays the slow op1000 path.
- base0c (crumb tree, same vars, ABL flag off) is byte-identical to the crumb arm
  (md5 582ae1d9/e515c5f4) -- the knob is neutral when off, so the sandwich's only
  variable was the skeleton block.

Reading: the number is not a skeleton ceiling, it is the price of fastloop removal.
The honest ceiling for a hand rewrite must keep the fastloop entry checks, which puts
the C-level pure saving at the floor already -- base0c minus skeleton is NEGATIVE.
skelceil3 (gates re-inserted, rest ablated) would separate the fastloop contribution
if that number is ever needed; the default conclusion is that the remaining SH-2
skeleton is not C-level reducible and the axis closes below the +10% threshold.

## 2026-08-25b: the audio stretcher defaults OFF -- a pure listening decision

Leader ruling after the isolation sandwich (aud3 vs triple, 2026-08-25):
stretcher fps cost is inside the +-1% session heat drift (34.850 vs 35.253
open / 34.065 close-valid / ~34.66 interpolated -- not separable), and the
anomalous crash ratio 1.5160 fired on a stretcher-OFF arm, so "freeze is
stretcher-specific" is dead. With no performance case either way, the default
is OFF: equal fps buys the removal of one ISR pull path and the PICOLA/retune
code, and two of the three anomalies were on stretcher arms (a weak signal,
not proof). The ported files live in the tree uncommitted and are parked in
/tmp/audring_backup/ -- turn it ON when a listener chooses pitch-correct audio
over the 66 ms ring-wrap echo it exists to remove.

## 2026-08-25: the clock ladder closed at 340, the audio ring is parked with one open freeze

Sandwich numbers today (all card-md5-verified, DOOM gameplay anchor):

- base0 26.81 -> tl2 32.58 (+21.5%, ym_tl_tab half-tables to DTCM)
  -> tl2oc2 33.30 -> triple 34.01 (+26.9% cumulative with OC2+22050).
- quad16k (16000 Hz audio) = +0.01% wash vs triple. The ar22k +2.00%
  lived entirely in the 44100->22050 half; below 22050 the sample
  savings cancel against the stretcher ISR + fixed audio overhead.
  22050 stands.
- oc348 (PLLN=174, pure clock A/B, byte-identical core) = -0.66% vs its
  quad16k bracket. Ladder final: 312 < 340 (peak, soak-passed) > 348
  (slower) > 353 (soak death). The old Genesis failure was 353/101, ours
  353/118 -- core 353 both times, so the wall is core-side, not OSPI.
  Clock axis closed at 340. GNW_OC2_PLLN is wired (default 170, neutral).

Closed without landing, all measured on the rig (insn/frame vs 7,230,349
baseline, snd hash identical throughout):

- ppgate (conditional ppc store): +0.023% = the store is free on M7
  (store buffer forwarding); the gate widening cancelled it. REJECTED.
- Write-bus fast paths: wwfast v1 (SDRAM inline) -0.15% noise; wwfast2
  (+DRAM quirks) +1.56% (macro bloat de-inlined the MOV* store helpers);
  wwtop (fast path inside p32x_sh2_writeN, ITCM) +0.38%. The store map
  path is already ~6 insn; write axis dead.
- GBR census: every one of the 1,363,400 0xC5 dispatches reads
  gbr_top=0x20 (32X comm space) -- real I/O work, not a lever.

SH-2 fixed cost (the number that picks the next lever): 49-51 of the
60.9 host insn/guest insn is the skeleton every opcode pays identically
(dispatch + fetch + PC update + icount + per-instruction poll check).
Per-opcode handler work is only 10-12. That size says a hand-written
dispatch (superinstructions or Thumb-2) is the only lever that can move
it; nothing C-level is left.

### The audio ring is PARKED -- read before resurrecting it

md32x_audio_stretch.c (SNES stretcher ported: ISR-pull, DTCM ring,
keep-pitch PICOLA) is in the tree but UNCOMMITTED and NOT for release.
It exists because sub-realtime emulation + free-running SAI DMA made the
read pointer lap the writer every ~66 ms: an echo at 44100, crackle at
22050. Freeze #1 (ITCM spray from a NULL ring during the warmup frame,
before the first reset) is fixed with a NULL guard. Freeze #2 is NOT
solved: an intermittent runaway (thread executing out of guest 68K RAM,
ISR dead first, thread survives seconds longer; happened at frame ~512
near music start, one boot in several; pointer/emu_owns watchpoints
clean over 3 min). If it is revived, start from the SAI-vs-push
preemption window and the PICOLA/retune path, not from a rewrite.

### The gate to shipping is stability, not fps

Three boot hangs on record -- tl2oc2, triple, and once on stock base0:
all emu=0 drawn=5-7, all fault registers zero, all before the boot-rescue
watchdog can see them. Not our levers; possibly not even 32X-specific.
Narrow the reproduction conditions before touching SH-2 for real.

## 2026-08-24: the FM data lever lands -- +21.5% from one table, and the sound side is now fully priced

The 08-22 campaign priced the whole sound path by ablation and every number
in it survived the device. What it left open was where the FM path's 8.4 ms
actually went -- the code was exonerated (ITCM'ing the renderer measured
zero) -- and the YM census answered: 735 samples a frame, 2.10 active
channels, 6.62 audible operators = 4,867 operator-samples, each ending in

    ret = ym_tl_tab[sin | (env << 7)];

against a 213 KB XIP blob. That is ~460 host cycles per operator-sample
where resident C costs 30-60.

**tl2** (picodrive `aac66f8f`, launcher `11599617` + `c127bca6`) computes
the value instead, from two 256-entry half-tables copied to DTCM at
PicoInit (1 KB total). The identity is proven both ways by
`tools/ym_tl2_proof.c`: the init_tables fill loops, ported verbatim,
regenerate the blob byte-for-byte against a dump out of the base ELF's
.rodata_md32x, and the computed form matches all 106,496 (even env, sin)
pairs. Device, Doom attract anchor, card-verified sandwich:

    base0 bracket   26.82 26.82 26.81 / 26.80 26.80 26.79    26.81
    tl2             32.60 32.57 32.56                        +21.5%

Audio bit-identical (rig snd hash unchanged). The linker drops the blob:
XIP shrinks 205 KB and `ym_tl_tab` leaves the ELF entirely.

Stacked with `GNW_OC2_PLLQ=6` (the soak-passed 340/113): **33.30, +24.2%
cumulative**. OC itself reads +2.59% on this base, down from +3.73% alone,
because tl2 already removed the OSPI fetches the clock was speeding up.
30-minute soak clean -- one boot-phase hang on the first attempt, the same
flake the stock clock showed once the same day, clean on the re-run.

**The sound side is priced.** gutcz80 (Z80 frozen at Cz80_Exec entry, sync
alive) reads 37.13; nofm (FM+PSG out) reads 34.49; base 26.76. So: FM render
~8.4 ms of which tl2 recovered 6.6, Z80 interpretation ~2.1 ms, YM synthesis
proper ~1.8 ms, PSG ~0.3 ms. The old "Z80 = 10.87%" was the 30 fps audio
anchor clipping the measurement, not a cost. Dead levers, each for a
measured reason: `lfo_pm_table` (44 loads/frame worst case = 0.06% --
access pattern, not size, decides a table lever), the 68K jump table (the
whole m68k bucket is ~2%), and anything that fits the 16 KB I-cache
(fmpath rule).

**Queue:**

0. `MD32X_AUDIO_RATE_OVERRIDE=22050` (`ebeae5fd` wired it; the ifdef and
   its "prices the axis" comment predate it). Everything in the sound
   bucket is per-sample, so one build halves YM remainder + PSG + PWM +
   mixer. Bench arm `ar22k` -- then a listening verdict, which no bench
   can give.
1. Re-run the YM census on a tl2 base to see what the remaining ~9 ms
   "rest of frame" bucket is made of now that sound is paid down.
2. Z80 slice skip stays priced at <= +6% and needs the pass-rate timer
   rescale (the 0x0921 immediate) the fold experiments did not obviate.
   SH2 hand-tuning is the only remaining big number, and it is a week,
   not a session.

## 2026-08-21b: the "no fast memory" answer was wrong, and the Z80 moved

The audit that closed both big axes said RAM_EMU had 1,516 B free, ITCM 1,684 B
and internal flash 5 KB, so Cz80_Exec's 17,892 B had nowhere to go. Every total
in it was right. Nobody had looked at what the 52,016 B of ITCM **data** was.

    .overlay_md32x_itc      13,188 B   code (sh2pico + sh2bus)
    .overlay_md32x_itc_bss  52,016 B   all data      -> 328 B free of 65,536

    fn_table              16,384 B   <- one static array in ym2612.c
    HighLnSpr              7,680 B
    SH-2 bus maps (x12)    9,216 B
    68K bus maps (x8)      8,192 B    <- half of them for a CPU not in this build
    ym2612                 3,216 B
    HighPreSpr             2,048 B
    DefOutBuff             1,280 B
    cz80_bad_address       1,024 B
    VdpSATCache            1,024 B

### fn_table was never a table

Every entry is `(UINT32)((double)i * C)` for one double `C` fixed in
`OPNSetPres`. A table of `i*C` is a multiply, and it is one now: `ym_fn_mul`
holds C in Q32 and `YM_FN(i)` is MUL+UMULL+ADD with no memory access.

`tools/ym_fn_table_proof.c` walks all 4096 entries for every (clock, rate) this
port can reach — NTSC and PAL, 22050/32000/44100/48000 and both native rates —
and finds **zero mismatches at Q32**. Q32 is also the only shift that works: Q20
and below fit a 32-bit multiplier but are wrong for some rates, Q34 overflows
the 64-bit product around i=3400. `OPNSetPres` re-runs the same comparison
against the double formula at every init, at the same cost as the fill loop it
replaced, so an unanticipated clock cannot pass silently.

End-to-end on the rig (`run_32x.sh`, Doom, 600 frames), pre-patch tree against
post-patch: **snd hash 97becd29 both, framebuffer checksums identical at f99 /
f299 / f599, identical SH-2 counts**, host insn/frame 6,226,060 vs 6,226,062.

**ITCM BSS 0xcb30 -> 0x8b38: 16,376 B freed.**

### Cz80_Exec now runs from ITCM

17,892 B needed, 16,704 B free. The last 1,188 came from `DefOutBuff` — the
1,280 B fallback line buffer, written sequentially per line, which does not need
zero-wait RAM. **No submodule edit was required**: input sections go to the
first output section that matches, and `.overlay_md32x_bss` is listed before
`.overlay_md32x_itc_bss`, so one line claims it.

Verified on the link, not by "it built": `md32x__Cz80_Exec` = `0x00003378`,
`DefOutBuff` = `0x24052a18`, and the only long-branch veneer left is for the
per-slice call into Cz80_Exec, not for anything per-instruction.

The five 1 KB jump tables stay in `.rodata_md32x` on purpose. They are read as
**data**, so they ride the D-cache, and the sound driver's opcode set is small
enough to stay resident in it. It is the instruction fetch that misses.

### And 4 KB more, for a Sega CD that is not in this build

`pico/memory.c` defined four 1 KB sub-68k maps under `GNW_32X_CORE` purely so
the dead `is_sub` branches would link — its own comment called them "4x1KB of
dead BSS, reclaimable later if RAM gets tight". They were in ITCM. The branches
are compiled out now, and a caller that passes `is_sub=1` is refused and logged
rather than silently steering the main 68K's map (which is what aliasing the two
symbols would have done). **ITCM headroom 88 B -> 2,136 B.**

Commits: picodrive `8ba2a962` + `f1d5d493` behind `74118703`.

### Measured on hardware

Doom, gameplay anchor, 1800 frames x 3 per arm, each against an adjacent
control. The control's own spread across three runs was 0.01-0.02 fps, so these
deltas are not noise.

| arm | runs | mean | vs adjacent control |
|---|---|---|---|
| `ctl1` control | 26.21 / 26.20 / 26.19 | 26.200 | — |
| `czitcm` fn_table + Cz80_Exec in ITCM | 27.13 / 27.12 / 27.12 | 27.123 | **+3.52%** |
| `ctl1b` control, second round | 26.14 / 26.16 / 26.16 | 26.153 | — |
| `fnq32` fn_table only | 26.24 / 26.23 / 26.22 | 26.230 | +0.29% |
| `s68kfree` + the sub-68k reclaim | 26.90 / 26.89 / 26.89 | 26.893 | +2.83% |

**Cz80_Exec running from ITCM is worth +2.8% to +3.5% on the device.** `fnq32`
lands where it should: freeing space is not a speedup, and the multiply is not
slower than the ITCM load it replaced.

Keep the ceiling honest: disabling the Z80 outright is +19.8%, and this
captures a fifth of that. The ablation removed the Z80's *work* as well as its
instruction fetch; only the fetch was ever available.

Next tenant for ITCM is priced but does not fit: `chan_render` is 3,260 B and
**6.3% of the frame**, the largest single XIP consumer in the census, against
2,136 B free. ITCM and RAM_EMU are both full again (2,136 B / 172 B). The only
untapped fast memory is the DTCM heap — `_heap_start 0x20003e70` to
`_heap_limit 0x20019f00`, 90,256 B — whose 32X-time high-water nobody has
measured. Measure it at the **worst case** (a large ROM list browsed, a non-Latin
language loaded, cheats parsed), not at a fresh boot: the launcher's own
`malloc` users are the i18n font cache (~9 KB), the language string buffer, and
the ROM/cheat lists, all of which scale with the card. Anything moved there
needs a fallback, because a `dtcm_malloc` at core load can fail on someone
else's card.

## The three things to do first

1. **Soak the clock levers.** `GNW_OC2_PLLQ=6` (340 MHz core / 113 MHz OSPI) is
   **+4.0%** and changes nothing else; level 3 + `GNW_OC3_PLLQ=6` (353/118) is
   **+8.4%**. Neither is a default. Level 3 was pulled from the launcher menu for
   Genesis instability under sustained load, and this core is that Genesis; 118 MHz
   is past anything this flash has been asked for, and 136 MHz hangs the console.
   30+ minutes of sustained load each, then decide.
2. **Listen to 22.05 kHz.** `MD32X_AUDIO_RATE_OVERRIDE=22050` is +1.9% stacked and
   nobody has heard it.
3. **Find 18 KB of fast memory.** It is the only lead left that reopens anything.
   Both remaining big axes — the Z80's 20% and the 30% of frame spent fetching from
   OSPI — are blocked on exactly this, and the audit is in issue #45: RAM_EMU has
   1,516 B free, ITCM 1,684 B, internal flash 5 KB, and the RAM overlay's entire
   text+rodata is 31.5 KB against `Cz80_Exec`'s 17.9 KB.

## Do not re-try (all measured on hardware, in the gameplay anchor)

| idea | result |
|---|---|
| `-Os` on `cz80.c` | **-3.9%** |
| `-Os` on `ym2612.c` | **-2.4%** |
| `-O3` on the SH-2 interpreter (`sh2pico.c`) | **-1.1%** |
| Z80 idle fold at every backward jump | **-6.9%**, and -6.1% of that is the hooks alone |
| Z80 idle fold per slice | never triggers — 0 folds in 116,694 slices |
| Z80 sync every 8th line | **0%** |
| OSPI 136 MHz (`PLLQ=5`) | console hangs |
| audio 44.1 → 22.05 kHz alone | +1.3% |
| `chan_render` (3,260 B, 6.3% of the frame) into ITCM | **0%** -- 26.903 against a 26.802 bracket, with 1.01% of drift across the same window |

The `-Os`/`-O3` trio is the one to remember: **this interpreter loses when it gets
smaller and loses when it gets bigger.** Do not spend another arm on compiler flags.

The `chan_render` line is the other one, and it corrects a claim this file
carried for an hour. The first measurement of that axis said the DTCM move was
-1.21%; it was taken with the card side unverified, and `/cores/32x.bin` is
part of the program -- every byte of ITCM code ships inside it. Re-measured
with `CORE=32x` hashing the card, the same arm read **+0.58%**, and
chan_render on its own read zero. Nothing about moving `HighLnSpr` to DTCM is
known to be harmful; nothing about chan_render in ITCM is known to help.

**The census's PC share does not predict what ITCM is worth. Size does.** A
small hot function is already resident in the 16 KB I-cache. Cz80_Exec paid
because 17,892 B of jump-table interpreter was evicting everything else from
it. Find the next block that big before spending an arm.

One result on this axis is still unexplained and is therefore a rule: an arm
that put chan_render in the MIDDLE of `.overlay_md32x_itc` and reordered
sh2pico's functions read **23.997** against controls of 27.163 and 27.160 --
-11.6% -- while the identical code appended at the end read zero. ILLEGAL was
not the cause (0 executions in 600 frames of Doom, counted on the rig) and
neither were veneers (the sets differ by one entry). **Append to that section;
do not reorder it; re-bench if you do.**

## Measure it the way it was measured

- **The anchor is `GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=0`.** Without them an arm
  cold-boots to Doom's title screen and reads ~32 fps instead of ~26, and a lever
  priced there is priced in a scene where the SH-2 is idle. `arm32x.sh` does not
  pass them; add them to every arm.
- **Bench each arm next to its own control.** The console drifted 26.25 → 25.74 →
  25.74 → 26.25 → 28.42 → 28.80 across one session. Two arms an hour apart are not
  compared at all.
- **Read counters over SWD without halting.** `openocd ... -c "init" -c "echo [mrw
  <addr>]"` and no `halt`: halting sends the console to sleep and freezes exactly
  what you were sampling. The on-device profiler's SD dump is what crashes; the
  counters themselves are fine.


## The 2026-08-21 map: where the 32X frame actually goes

First device PC census of this core (600 samples, halt/read-pc/resume on the
real chip, Doom):

    46.7%  sh2_execute_interpreter        ITCM
    30.0%  executing out of OSPI flash    chan_render 6.3, Cz80_Exec 5.5,
                                          DrawLayer 2.3, m68k_run 2.0,
                                          TileNorm 1.5, FinalizeLine555 1.2,
                                          PicoLine 1.0, SN76496Update 0.8

**Nearly a third of the frame is instruction fetch across the QSPI bus**, and
that is the finding the rest of the day came out of.

### Priced on hardware, in the gameplay anchor, each against an adjacent control

| lever | anchor fps | delta | state |
|---|---|---|---|
| control (340 MHz core / 97 MHz OSPI) | 26.25 → 25.74 | — | drifted -1.9% over the session |
| **340 / 113** (`GNW_OC2_PLLQ=6`) | 27.30 | **+4.0%** | landed, off by default, wants a soak |
| **353 / 118** (level 3 + `GNW_OC3_PLLQ=6`) | 27.89 | **+8.4%** | landed, off by default, level 3 has an instability history |
| Z80 disabled (ablation) | 31.44 | +19.8% | **not shippable** — Doom's music is a Z80 driver |
| FM+PSG disabled (ablation, menu scene) | — | +4.4% | not shippable |
| `cz80.c` built `-Os` | 25.22 | **-3.9%** | closed |
| Z80 sync every 8th line, not every odd | 32.10 vs 32.08 | 0% | closed |
| audio 44100 → 22050 | — | +1.3% | not worth the quality |

PLLQ=5 (136 MHz OSPI) hangs the console outright, so the flash ceiling is real
and close to 118.

### The Z80 is the prize and it is not a spin loop

Disabling it is worth **+19.8%** in the anchor, which is more than its own
share of the frame — removing it also stops it evicting everyone else from a
16 KB I-cache. But it cannot simply be folded:

- Z80 guest-PC census over SWD (400 samples, `CZ80.PC - CZ80.BasePC`): 35
  distinct PCs, `0x02b7` 27.8%, `0x0049` 21.8%, `0x0906`/`0x090d` 23.3%.
- Disassembled from Z80 RAM over SWD, that is a **cooperative polling sound
  driver**, not an idle spin: a main loop at `0x0900` that calls an empty
  `CALL $02B7` → `C9 RET` hook every pass, calls a flag-check at `0x0041`
  (`LD HL,$003F; LD A,(HL); OR A; JR Z`) between every stage, and decrements a
  tempo counter at `$0040`. **It writes memory every iteration**, so the 68K
  idle-fold's "no side effects" whitelist rejects it, and skipping iterations
  changes the music tempo.

The arithmetic says why it is so expensive: ~17.5k Z80 instructions per frame
taking ~20% of a 38 ms frame is **~148 host cycles per Z80 instruction**, and
one 32-byte I-cache line refilled from OSPI at 97 MHz costs ~224 core cycles.
`Cz80_Exec` is 17.9 KB of flash-resident interpreter with a jump table: it is
missing roughly once per instruction.

**So the fix is to stop fetching it over QSPI, and there is nowhere to put
it.** Checked, not assumed:

- RAM_EMU headroom is 1,516 B; its 708 KB of BSS is `gnw_32xmem` (532 KB: 32X
  SDRAM + both framebuffers) and `PicoMem` (140 KB: 68K RAM, VRAM, Z80 RAM).
  All of it is the emulated console's own memory.
- ITCM is 63,852 of 65,536 used — 11,836 B of text (the SH-2 dispatcher) and
  52,016 B of BSS (YM, VDP, both bus maps, cz80, z80if state).
- The RAM overlay's whole `.text`+`.rodata` is 31.5 KB. `Cz80_Exec` alone is
  17.9 KB, so it cannot be traded in.

That leaves **HLE of the Z80 sound driver** as the only route to the Z80's
~20%, which is a project, not a session. Everything cheap on this axis has
been tried and is in the table above.


**The result is landed and released.** Device +13.1% (compounded from
adjacent-arm deltas), branch `testbed`, four levers all hash-identical, shipped
as `testbed-full-20260820-1854`. The arm on the card (`wb1`) is still a
measurement build (autoboot + savestate resume), not the canonical flag set --
so a device bench continues against `wb1`, not against the release.

## 1. DONE: the release is out, from a green tree

**`testbed-full-20260820-2101`** (tag on `89efdef8`) — `retro-go_update.bin` +
`gw_update.tar`. The +13.1% is in the field. Provenance checked rather than
assumed: the tag records picodrive `dee15af6`, and all five lever commits
(`00a11484 da121a68 011f8c5c 6a997eb4 ab232912`) are ancestors of it; the
shipped `cores/32x.bin` is 44,507 bytes, i.e. post-compositor-fold (pre-fold was
45,833).

An earlier tag, `testbed-full-20260820-1854`, carries the same firmware but was
published from a tree that failed two gates. It is kept (releases accumulate),
and `-2101`'s notes say which one to flash.

**Why a red tree could publish at all:** `release-package` had no `needs:`. A tag
published a release no matter what the tests said, so when `ef03bfc9` (nhdoom)
broke two jobs on 0818, releases kept going out and the red X became the repo's
normal state — for two days, unread. Both failures are fixed in `461ce0be`:

- `build-sd0-test`: nhdoom's overlay sections exist only in
  `STM32H7B0VBTx_SDCARD.ld`, so the SD_CARD=0 link buried DTCM and reported it
  as `FLASH.ld:1142 cannot move location counter backwards` — coordinates in a
  linker script, naming neither the core nor the commit. `USE_NHDOOM` follows
  `SD_CARD` now, and the launcher's Doom branch moved into `launch_nhdoom()`
  behind the same guard (it was reading `app_main_nhdoom` and three
  `_OVERLAY_DOOM_*` symbols from outside the `#ifdef`).
- `host-tests`: `nhdoom/src/device_video.c` was not in the lcd_swap audit list.
  Audited as always-draws — `startDisplayRefresh()` is the engine's
  `I_FinishUpdateBlock` hook, so it only runs after a frame was rendered.

And the gate is closed: `89efdef8` gives `release-package`
`needs: [host-tests, lynx-core-test, build-sd0-test]`. Verified end to end — the
`-2101` run waited for all three and then published.

**Judge a release by its job, not by the run.** Even now, read
`gh run view <id>` per job: a green run is not the claim you care about.

## 2. The profiler's boot death is fixed. A second fault remains.

2026-08-21, on hardware, after the power cycle this file was waiting for.

### What it was: `ahb_calloc` is not an AHB allocator

`gw_malloc.c`'s `ahb_calloc()` calls `ram_malloc()` FIRST and only falls back
to AHB. RAM_EMU is 99.8% md32x's own overlay, so `ram_malloc` hands out memory
**inside the running core**. This file's own diag line had been saying so since
the day it broke -- nobody had read it on a *failing* boot:

    profiler init: drawn=0x240ffc84 skip=0x3001d0a8 active=1

Two pools, two memories. `skip` is AHB SRAM; `drawn` is RAM_EMU, a few hundred
bytes under `_OVERLAY_MD32X_BSS_END` (`0x240ffff4`). The profiler spent every
run writing per-frame deltas over the core's own BSS. **That is why four runs
gave four fault classes**: a wild write does not have a failure mode, it has
whatever the corrupted byte does next.

It also disarmed the guard the file was written against. The comment above
`MD32X_PROFILE_FRAMES` said `ahb_calloc` "asserts inside `ahb_only_malloc`" on
overflow -- so the AHB-exhaustion hypothesis was tested and correctly refuted,
while the real overflow was going to RAM the whole time. It never reaches that
assert: `ram_malloc` succeeds first. **An allocator that cannot fail is not a
bound.**

Fixed in `fdc85fc4` (`prof_ahb_calloc`, AHB only). After:

    profiler init: drawn=0x3001d0b4 skip=0x3001d3b4 active=1 ahb_free_after=1436

### The detector to keep: a warm start, not a power cycle

**An SWD `start` with no power cycle was a reliable killer, and that is worth
more than the power cycle was.** `prof1` died four times running, byte for
byte, at three different fault classes, while the shipping arm `wb1` booted
from the same card and the same savestate every time. So the failure had a
two-minute reproduction that needed nobody in the room, and the fix proved
itself against it: the same warm start now boots, resumes and runs.

Why a power cycle papered over it was not chased. It changes what RAM_EMU
holds at boot; the write was out of bounds either way.

### What is still broken

The profiler now **boots**. It still dies **later**, once the window opens
(1200 warmup frames -- ~20 s at the title screen, ~60 s in gameplay), and only
in the gameplay scene:

| arm | scene | outcome |
|---|---|---|
| prof7 (alloc fix) | title, msh2 idle | ran, dumped 3,200 B |
| prof7 (alloc fix) | gameplay | **Usagefault UNDEFINSTR, PC=0x0000001a** |
| prof8 (alloc + printf fix) | gameplay | **Busfault PRECISERR, PC=0x08127db6 in `_vfiprintf_r`, BFAR=0xdeb44a00**, byte-identical across two runs |

Note what the PCs say: `0x1a`, `0x28`, `0x48` are *vector table* addresses.
Something is still writing to very low memory, or corrupting a return address.

Already exonerated, do not re-test:

- **the pcwall probe** (`sh2pico.c:gnw_pcwall_sample`) -- indices are guarded
  by `off < GNW_PCWALL_WIN_SIZE`, unsigned wrap included, and
  `gnw_sh2_pcwall_arm` sets `armed` only after a non-NULL block.
- **the m68k probe** (`m68kcpu.c:gnw_m68k_sample`) -- same shape, same guards.
- **the `%llu` printf** -- prof7 does not have that fix and dies too.
- **the savestate and the card** -- `wb1` resumes the same `.sav` fine.

Where to look next: prof8's fault is **deterministic and precise** (same PC,
LR, CFSR and BFAR twice), which is a far better starting point than the four
random classes this began with. Disassemble `0x08127db6` (0x76 into
`_vfiprintf_r`) and see which argument register holds `0xdeb44a00`; and note
that FatFs does not commit a truncation until `f_close`, so a dump that
crashes mid-write leaves the PREVIOUS run's file intact on the card -- that
cost an hour here, reading prof7's dump believing it was prof8's.

### The sub-phase section has never printed a correct number

`%llu` does not work here: nano.specs' printf does not parse the `l`/`ll`
length modifier, so it emits the literal characters `lu` and consumes **no
argument** -- every later value on the line shifts one left. On device that
reads as `pico_total(outside)=lu` and `pct_of_pico=316688233.4%`. The counters
were never wrong; the line was. `%llu` had been introduced the day before to
fix a real 32-bit truncation (an 8.5 G sum printed as 4.2 G, plausibly), so
both spellings were wrong. `prof_u64()` renders the digits by hand and hands
printf a `%s`.

**Every sub-phase number ever quoted from this profiler is suspect.** The
top-level buckets (pace/proc/pico/blit/audio) use `%u` and are sound.

## 3. The A/B loop still works, but you are aiming blind without the profile

`wb1` benches normally -- **26.58/26.60/26.58 = 26.587 at the device's new
physical location** (it was moved; the old 26.68 is not comparable). So any
lever can still be judged on hardware. But msh2 is closed all the way down to
per-opcode body cost, and the remaining rig buckets are small, so choosing the
next target without a fresh device profile is the mistake this file keeps
warning about.

## 4. Device work: one owner

The second agent session was shut down. It overwrote a flash mid-experiment,
which then produced a wrong reading (`wb1` measured and reported as `prof4`).
One owner of the device removes that whole class of error; bring a second
session back only for adversarial verification at verdict time.

---

# 2026-08-20 — +10.5% on hardware, and a measurement floor nobody had measured

**Device, Doom gameplay anchor, 1800-frame runs:**

| time | arm | drawn fps | what it is |
|---|---|---|---|
| 07:54 | `baseline24` | 24.01 | picodrive `48d3f5b6` |
| 08:09 | `disp1` | 25.46 / 25.46 / 25.44 | + SH-2 dispatch cuts |
| 08:25 | `nrd1` | 24.71 / 24.71 / 24.69 | + solid-run detector REMOVED -> rejected |
| 09:03 | `base2` | 25.12 / 25.11 / 25.09 | today's tip, both new levers **off** |
| ~09:15 | `idl2` | 26.53 / 26.54 / 26.54 | base2 + **68K idle fold** |
| ~09:30 | `disp1` again | 24.95 / 24.95 / 24.95 | **control** |
| ~10:2x | `ppo1` | 26.35 / 26.34 / 26.34 | idl2 + **compositor folded to one copy + octa** |
| ~10:3x | `idl2` again | 26.17 / 26.16 / 26.16 | **control** (was 26.54 at 09:15, -1.4%) |
| ~11:1x | `ppo1` | 26.65 / 26.63 / 26.62 | sandwich, before |
| ~11:2x | **`wb1`** | **26.69 / 26.68 / 26.67** | + **eight-pixel solid-run blast** |
| ~11:3x | `ppo1` | 26.65 / 26.64 / 26.64 | sandwich, after |

**Add the controlled deltas, do not subtract the endpoints.** 24.01 at 07:54
against 26.68 at 11:2x is +11.1%, but those endpoints are four hours apart on a
device that wanders. Each lever measured against an arm benched beside it:

    disp1 / baseline24   +6.04%
    idl2  / base2        +5.68%     (base2 == disp1, established by control)
    ppo1  / idl2         +0.69%
    wb1   / ppo1         +0.16%
                         -------
    compounded           +13.1%

and the raw endpoints read lower than that because the device happened to be
2% down when `idl2` was measured and back up by `wb1`. **The drift is not a
trend you can subtract; it is a wander you have to control for.** This session
it was +0.01 -- essentially none -- against -2.0% in the morning.

Two results, and the second one is the one to carry forward.

## ★ This device drifts ~2% an hour, so an A/B is only worth its control

`disp1` measured **25.46 / 25.46 / 25.44** at 08:12 and **24.95 / 24.95 /
24.95** at 09:30 -- the same binary, the same card (md5 `70f8112c` re-verified
off the device both times), the same anchor. **-2.0% of session drift**, with
a spread of 0.00 at the second reading, so the drift is not noise.

That is larger than most of the levers this project argues about, and it
explains the gap that nearly cost a wrong conclusion today: `base2` came in
1.3% under `disp1` and looked like evidence that 32 bytes of dead-code layout
move the frame rate. They do not. `base2` and the re-benched `disp1` agree
inside the drift, so **today's tree change is neutral** -- which is what the
control was run to establish.

**Rules that follow, and they are not optional:**

- Two arms compared across an hour of session are not compared at all. Bench
  them adjacent.
- **Re-bench the reference arm at the end.** If the control has moved, every
  delta in that session has to be read against the movement, not against the
  morning's number.
- A tight spread (0.02) says the *binary* is reproducible. It says nothing
  about whether the *device* is where it was an hour ago.

## The 68K idle fold: rig -2.74%, device **+5.6%**

`idl2` 26.537 avg against `base2` 25.107 avg -- **+5.68%** -- twelve minutes
apart, one build flag between them (`GNW_M68K_NO_IDLE_FOLD`), card md5s
`a5826711` and `6d5f3166` verified off the device. The rig called this lever a 2.74%
*instruction* reduction and it is worth more than twice that on hardware --
predicted, for once, rather than discovered: the 68K is **3.5% of the rig
frame and 12.9% of the device's**, so the rig underweights that bucket by
about 3.7x.

Baseline to best, today: **24.01 -> 26.54, +10.5%** -- and the drift says the
true figure is a little larger than that, since the 26.53 was measured into a
device already 2% down on where it started.

## The compositor: eight copies folded into one, +0.69% and 1,472 bytes back

`do_line_pp` was a macro, expanded eight times (six `make_do_loop` variants
plus two in `FinalizeLine32xRGB555`), and every copy landed in RAM_EMU because
the linker script pins `PicoDraw32xLayer` / `PicoDraw32xLayerMdOnly` /
`FinalizeLine32xRGB555` there and the `do_loop_*` statics inline into them.
**That is where this core's headroom went**, and it is why the "2,604 bytes"
of solid-run detector and "592 bytes" of octa path were never really 2,604 and
592 -- they were about a hundred bytes each, times eight.

The variants differ in one thing: what to write where the MD layer is not
background, and that has three forms. It is an `md_mode` parameter now
(picodrive `011f8c5c`), and `.text.gnw_line_pp` is named in
`STM32H7B0VBTx_SDCARD.ld` so the compositor does not fall into `MD32X_CODE`.

    _OVERLAY_MD32X_BSS_END  0x240ffff4 -> 0x240ffa14
    free                    44 B       -> 1,516 B
    32x.bin                 45,833     -> 44,361     (with octa back ON)

The octa path costs **160 bytes** now instead of 592, because there is one copy
of it. Device: **`ppo1` 26.343 avg against a same-session `idl2` control of
26.163, +0.69%**, hashes bit-identical.

Independent check worth copying: the gap between `FinalizeLine32xRGB555` and
`PicoDraw32xLayer` in the ELF went from `0xFF4` (~4.1 KB) to `0x2E4` (~740 B).
That is the copies collapsing, read straight off the symbol table rather than
inferred from a size total.

## ★ Sharper: the rig under-prices wide stores. That one sentence covers all four.

Four rig/device pairs today, and the earlier phrasing ("pixel-path levers get
the wrong sign") was too broad -- `ppo1` is a pixel-path lever and the rig got
it right, in magnitude as well as sign.

| lever | rig (instructions) | device (fps) | |
|---|---|---|---|
| `disp1` SH-2 dispatch cuts | -6.67% | **+6.04%** | agree |
| `idl2` 68K idle fold | -2.74% | **+5.68%** | agree, rig 2x low |
| `ppo1` fold + octa | -0.71% | **+0.69%** | agree, near-exact |
| `nrd1` remove the run detector | -0.48% | **-2.95%** | **reversed** |

QEMU has no cache and no write buffer, so a wide store costs it the same as a
narrow one. Therefore it **overvalues removing** wide stores (`nrd1`) and
**undervalues adding** them (the octa half of `ppo1`). Everything else it
prices honestly, and where it is wrong about a bucket's weight rather than an
instruction's cost -- the 68K is 3.5% of the rig frame and 12.9% of the
device's -- it is wrong by a factor you can compute in advance from the phase
table, which is exactly what happened with `idl2`.

# 2026-08-20 — the SH-2 dispatch overhead, and what the rig can and cannot predict

**Device: 24.01 -> 25.46 drawn fps (+6.04%), 3 samples, spread 0.02.**
Baseline arm `baseline24` (picodrive `48d3f5b6`), new arm `disp1`
(picodrive `00a11484`), same session, card `cores/32x.bin` md5s verified
different (`70f8112c` vs the baseline's).

## What shipped

Four cuts to the SH-2 interpreter's per-dispatch bookkeeping. Direct-path
bookkeeping went from **22 host instructions per dispatched guest
instruction to 15**; none of them changes what the interpreter computes.

| # | change | rig | commit |
|---|---|---|---|
| 1 | `BUSY_LOOP_HACKS` compiled out for `GNW_32X_CORE` | -1.79% | picodrive `6889be8e` |
| 2 | fastloop nibble gate moved into the switch cases it duplicated | -3.38% | same |
| 3 | pc round trip and the redundant `delay = 0` store removed | -0.45% | same |
| 4 | opcode fetch window hoisted out of the per-instruction path | -1.20% | picodrive `00a11484` |
| | **compounded** | **-6.67%** | device **+6.04%** |

(1) is the one worth remembering. Both `BUSY_LOOP_HACKS` blocks in
`mame/sh2.c` compare the opcode at `sh2->ppc` against the one they expect to
*follow* them -- but `ppc` in this dispatch loop is the executing
instruction's own address, so neither comparison can ever be true.
sh2pico.c's own fastloop comment already said "inert here". Inert, and still
issuing a guest 16-bit read on every dispatched `DT` and every `BRA`.

(4)'s safety argument is a property of the code, not an assumption:
`gnw_fw_rom` is written only by `bank_switch_rom_sh2()`, whose callers are
`PicoMemSetup32x()` and `p32x_update_banks()`, and everything it derives from
is fixed at cart load. Neither can run inside `sh2_execute_interpreter`. A
future runtime writer breaks it silently -- the comment says so, and
`-DGNW_FW_NO_HOIST` is the escape.

## ★ The rig predicts one kind of lever and gets the sign wrong on the other

This is the most useful thing today produced, and it now has three data
points on each side.

**Instruction-removal levers in the SH-2 interpreter map correctly.**
`disp1`: rig -6.67% -> device +6.04%.

**Pixel/memory-path levers do not just miss the magnitude, they miss the
sign.** QEMU has no cache and no wait states, so it cannot price a wide
store against the per-pixel work it replaces:

| lever | rig said | device said |
|---|---|---|
| blank nametable row cache | -1.82% (a gain) | **-1.46% fps** (2026-08-19) |
| removing the compositor's solid-run detector | -0.48% (a gain) | **-2.95% fps** (arm `nrd1`, 24.71 vs 25.46) |

`nrd1` is the sharper of the two, because the run detector's whole job is to
replace per-pixel `u16` stores with `u32` blasts. On the rig those blasts are
just instructions and the detector's failed probes are pure tax, so removing
it "wins". On the device the blasts are the point. (`nrd1` = 24.71/24.71/24.69,
spread 0.02, card md5 `729639b1` against `disp1`'s `70f8112c`.)

## The RAM_EMU headroom is tens of bytes, and it moves with the code

Read it out of the ELF for the exact tree you are building -- the build log
never prints it, and the number is not a constant:

    arm-none-eabi-nm <elf> | grep _OVERLAY_MD32X_BSS_END
    __RAM_EMU_END__ = 0x24100000

| arm | BSS end | free |
|---|---|---|
| `baseline24` (`48d3f5b6`) | 0x240ffff4 | 12 B |
| `disp1` (`00a11484`) | 0x240ffff4 | 12 B |
| `base2` (today's tree, both new levers off) | 0x240fffd4 | 44 B |
| `nrd1` (solid-run detector removed) | 0x240ff5d4 | 2,604 B |

**Plan against tens of bytes, and measure the tree you are about to link.**
Quoting one figure is what went wrong twice today: the 44 in `32X_CLOSED.md`'s
blank-row row was a different tree's, and sizing a cache to it failed the
link; sizing the next one to `disp1`'s 12 was right for that tree and
pessimistic for this one.

That turned two of today's levers into budget questions rather than
performance ones:

- The 68K idle fold needed a verdict cache. A struct-per-slot version cost
  260 B and failed the link; four one-word slots cost 16 B and failed it by
  four; **two slots, eight bytes, fits**. The fold's *code* costs nothing
  here -- `gnw_m68k_idle_probe` links at `0xdeb086d0` and the whole gwenesis
  core at `0xdeb29910`, i.e. XIP.
- **The octa composite path does not fit and is shelved.** Not on merit: its
  ~150 B body is instantiated once per `make_do_loop` variant inside
  `PicoDraw32xLayer`, which the linker script pins in RAM_EMU, and the total
  came to **592 B** against twelve.

And it gave the compositor a price tag in the other currency: removing the
solid-run detector frees **2,604 bytes** of RAM_EMU (`nrd1` links with
`_OVERLAY_MD32X_BSS_END = 0x240ff5d4`). So the detector is a standing trade of
2.6 KB of the scarcest resource this core has for +2.95% device fps -- worth
keeping, and the obvious place to look for bytes if something else needs them.
### Where the bytes are: `do_line_pp` is compiled six times

`make_do_loop` is instantiated **six times** (`draw.c:519-524`: bare, `_md`,
`_h32`, `_scan`, `_scan_h32`, `_scan_md`), and each instantiation expands
`do_line_pp` -- solid-run detector, quad path, pair path, the lot -- in full.
That is where 2,604 bytes of run detector and 592 bytes of octa come from:
about 100 and 434 bytes of actual code, times six.

The variants differ in exactly two things: a `pre_code`/`post_code` pair
(`PicoScan32xBegin/End`) and a `md_code` that has three forms (empty,
`MD_LAYER_CODE`, `MD_LAYER_CODE_H32`). **Turning `do_line_pp` into one real
function taking a three-valued `md_mode` would collapse six copies into one**
and hand back on the order of 2-3 KB of RAM_EMU -- which is not "some
headroom", it is the first real headroom this core has had, enough for the
octa path several times over.

What it costs, and why it needs a device A/B rather than a rig one:

- one branch on `md_mode` per *non-background* pixel. For Doom's 3D view that
  is the cold half of the loop (the MD layer is background there), but the HUD
  region -- lines 80..223 by today's census -- pays it.
- the loop stops being specialised per variant, so gcc loses some constant
  folding.
- it is store-traffic-adjacent code, and this tree now has two measurements
  saying the rig gets the sign wrong on exactly that.

Do it, link it, read `_OVERLAY_MD32X_BSS_END` out of the ELF, then bench
against `disp1`. If it is neutral on fps it is still a large win, because what
it buys is spendable.

**The rule that follows:** a 32X lever that removes instructions from the
SH-2 interpreter may be proposed on rig evidence. A lever that changes how
many bytes move, or in what width, may not -- it must be benched on the
device, and the rig's sign is not even a hint. Both directions are now in
`32X_CLOSED.md`'s ledger.

A corollary nobody has cashed in yet: **the rig has therefore been
under-pricing the compositor's wide-store paths all along.** The quad
composite path was measured at 0.003% on the rig and dismissed; on the
device it may be worth several percent. Re-pricing `-DGNW_PP_NO_QUAD` on
hardware is a one-flag arm and is the first thing to do if the compositor is
revisited.

## The 68K was spending three quarters of itself in one spin

New instrument, `RIG_M68K_HIST` (picodrive `2a28aed8`, exact-PC census for
the 68K). On Doom's gameplay anchor, **75.2% of every 68K instruction
executed** is in one two-instruction loop:

    0x8832a2  tst.b  $ff1134.l      ; a flag in the 68K's own work RAM
    0x8832a8  beq.b  $8832a2        ; wait for the VInt handler to set it

That is `SekIsIdleCode`'s 6-byte `tst.b ($xxxxxxxx)` case, verbatim.
picodrive has carried upstream's idle-loop whitelist all along -- and wired
it to Cyclone and FAME only, by patching the branch opcode. **gwenesis
(`EMU_G68K`), which is what this build runs, was never wired to any of it**,
and the 32X-register poll detect that does call `SekSetStop` cannot see this
loop because the address polled is RAM, not a 32X register.

`GNW_M68K_IDLE_FOLD` (picodrive `2a28aed8`) detects at the branch instead of
patching: a taken short backward `Bcc`/`BRA` whose body passes
`SekIsIdleCode`, verdict cached per branch target so the check runs once per
site and never inside the loop. It sets the stop flag directly rather than
going through `SekSetStop`, whose `SekEndRun` rebases `Pico.t.m68c_cnt` --
rewinding the master clock is not what a spinning guest does. It never
sleeps with interrupts masked at or above VInt's level.

    rig  9,901,391 -> 9,630,064 host insn/frame   -2.74%
    68K guest insns 8,845/frame -> 2,195/frame    -75.2%

Framebuffer hashes bit-identical, guest SH-2 instruction count identical.
**Device verdict pending** (arm `idl1`).

**The audio hash moves, and that needed a new instrument to interpret.**
`af8c118b -> 8c20ba95`, on a pixel-identical frame. The VInt handler starts
up to one scanline later, so every sample boundary downstream shifts. The
rig now also reports audio *energy*, which survives a time shift and does not
survive a change of content:

    sum|s|    37,682,184 vs 37,690,020    -0.021%
    sum s^2   231,270,959,840 vs 231,289,673,356   -0.008%
    peak      32767 vs 32767              identical

Energy does not replace the hash -- an unmoved hash is still the only proof
of bit-identity -- but the pair separates "rescheduled" from "broken".

After the fold the 68K's hottest code is a *called helper*, not a spin:

    0x882d6a  move.w (a2), d3
    0x882d6c  cmp.w  (a2), d3
    0x882d6e  bne.b  $882d6a        ; read a volatile word twice until it agrees

79 calls a frame, exits on the first compare almost always. Do not fold it:
the second read is the point.

## Where the frame stands after all of it

Rig, same anchor, `PHASE_PROF=1`, everything in (dispatch set + 68K idle fold
+ octa composite):

| phase | host insn/frame | share | was (morning) |
|---|---|---|---|
| msh2 (interp+bus) | 5,621,702 | 58.7% | 6,202,186 |
| ssh2 (interp+bus) | 1,101,021 | 11.5% | 1,227,275 |
| draw (MD VDP line) | 968,680 | 10.1% | 968,677 |
| 32x (compositor) | 668,892 | 6.9% | 766,770 |
| fm (ym2612) | 350,434 | 3.6% | 350,433 |
| **m68k (interp+bus)** | **340,250** | **3.5%** | **610,911** |
| z80 | 218,968 | 2.2% | 218,994 |
| other (sched/ev/mem) | 173,580 | 1.8% | 154,464 |
| snd (psg+dac+mix) | 113,391 | 1.1% | 113,400 |
| **TOTAL** | **9,565,182** | | **10,621,382** |

SH-2 host-per-guest instruction: **78.559 -> 71.086**.

Read this with **ssh2 subtracted** -- it is 11.5% of the rig and measured
**0.2%** of the device. And read `draw` with suspicion in the other direction:
it is 10.1% here and the July device profile put the whole draw axis near 2%,
which is consistent with the blank-row cache doing nothing good on hardware.

The two buckets that remain worth attacking on the *device* are msh2 (58.7%
here, ~70% there) and the compositor (6.9% here, and the `nrd1` result says
the device charges more than that for its store traffic).

## The one lever the rig could not judge at all, and the device could

The solid-run blast wrote four pixels a round; it writes eight now. The rig put
it at **exactly zero** (9,689,700 against 9,690,235) -- not imprecision but
structural inability, since QEMU has no write buffer and prices a wide store
like a narrow one. Sandwiched on the device it is **+0.16%**, with every `wb1`
sample above all six `ppo1` samples (complete separation, not an optimistic
read of overlapping ranges). 160 bytes, hashes bit-identical. picodrive
`ab232912`.

Worth keeping as the clean example: when the rig reports 0.00% on a
store-shaped change, that is the rig declining to answer, and the arm is still
worth building.

## The interpreter's op bodies are flat -- there is no expensive opcode to find

`RIG_OPCOST` (new instrument) brackets the dispatch switch with the rig's
instruction clock and books the delta to the top nibble, so a *count* share can
be checked against a *cost* share. msh2, Doom gameplay anchor, ratios only (the
probe inflates the total by ~37%):

| group | share of op-body time | avg | n |
|---|---|---|---|
| 0x8 BT/BF | 35.65% | **4.2** | 748,268 |
| 0x4 shift/DT/JSR | 14.02% | 1.1 | 1,109,353 |
| 0x6 loads | 9.40% | 0.7 | 1,250,266 |
| 0x0 NOP/RTS | 8.37% | 0.6 | 1,180,276 |
| 0x3 ALU | 7.82% | 0.8 | 836,127 |
| 0x2 stores | 7.03% | 0.9 | 692,594 |
| 0xD MOV.L @(d,PC) | 5.25% | 0.8 | 564,433 |
| 0xC misc/imm | 5.24% | **3.3** | 142,473 |

**Read it as a negative result, which is what it is.** Loads cost 0.7 and stores
0.9 -- the inline fast paths are doing their job, and there is no opcode whose
body is worth hand-tuning. Group 0x8's 4.2 is not a target either: it is where
`gnw_sh2_fastloop` is called from, so a fold that collapses hundreds of guest
iterations is booked to the one dispatch that triggered it. That is the shadow
of a lever already won.

**The one outlier, chased and closed.** Group 0xC at avg 3.3 is
**70.3% `MOV.W @(disp,GBR),R0`** plus 10.5% `MOV.L @(disp,GBR),R0` -- guest
loads through GBR, which is the SH-2 idiom for a hardware-register base. They
miss both inline fast paths (`RW` covers cart ROM and SDRAM; `0x20004000 &
0xdf000000` is neither) and call `p32x_sh2_read16`. So they are peripheral
reads, the most expensive thing measured on this device at **106.5 cycles**.

The arithmetic that closes it: **911 of them per frame on msh2**, times 106.5,
against a frame that is 12.8 M cycles at 340 MHz and 26.5 fps -- **0.76%**.
Real, and not worth building for.

## Closed today, by measurement

- **NOP short-circuit.** The opcode census (`RIG_OPHIST`, picodrive
  `e9fcda4b`) says msh2 spends 12.08% of its dispatched instructions in the
  `0x00xx` group, 64.7% of which is NOP -- 7.8% of everything -- and 28.0%
  RTS. Short-circuiting NOP ahead of `op0000`'s 64-way second-level switch
  measured **0.10%**. gcc's jump table for that switch is already cheap; what
  a NOP pays for is the loop iteration, not the sub-dispatch, and removing
  the iteration costs more in the delay-slot path than it saves.
- **Carrying `delay`/`test_irq`/`ppc` in locals** (arm A5): **+3.14%, a
  regression.** The dispatch loop is at its register ceiling; one more live
  value and gcc spills. Do not "optimise" this loop by adding locals.
- **The MD sound subsystems are not free work.** `-DRIG_NO_FM` is worth
  -3.54% and `-DRIG_NO_Z80` -4.30% on the rig, and **both move the audio
  hash**, so Doom 32X genuinely drives the YM2612 and the Z80. There is no
  silent chip to switch off.

## The compositor has been undervalued all along, and there is a much bigger version of that idea sitting in the firmware already

Re-pricing the quad composite path on the gameplay anchor with today's tree:

    -DGNW_PP_NO_QUAD    9,919,359 vs 9,630,064   +3.00%

**3.00%, not the 0.003% on record** -- that number came from a different
anchor. And since the rig under-prices wide stores, the device figure should
be at least that. So the compositor's blast paths are worth more than anyone
here has assumed, and going wider is worth doing rather than arguing about:

    octa (8 px/round)   9,630,064 -> 9,532,189   -1.02%   picodrive 2bc61933

Hash-gated bit-identical. `-DGNW_PP_NO_OCTA` prices it.

### The big one: the LCD can do the palette lookup in hardware

`do_line_pp`'s inner work, for every pixel where the MD layer is background,
is *one palette lookup and one 16-bit store*. That is a CLUT expansion done
in software into an RGB565 framebuffer -- and **the LTDC can do exactly that
in hardware, and this firmware already has the code**:

    Core/Src/gw_lcd.c:338  lcd_setup_framebuffers(LCD_MODE_LUT8)
                           -> LTDC_PIXEL_FORMAT_L8 + HAL_LTDC_ConfigCLUT
    Core/Src/gw_lcd.c:405  lcd_get_bonus_pool()

It was built for PICO-8, it is exposed through the SD-core ABI
(`Core/Src/retro-go/gw_firmware_abi.c`), and it does two things a 32X core
would very much like:

1. **The per-pixel palette lookup and the 16-bit store both disappear.** The
   compositor becomes a byte copy for the 32X's packed-pixel mode, resolving
   the DRAM line-offset table per line. The `32x` phase bucket is 7.2% of the
   rig frame and, being pure memory traffic, more than that on the device.
2. **It halves the framebuffer footprint: 300 KB -> 154 KB, freeing 146 KB.**
   For a core whose overlay BSS has had **44 bytes** of headroom, that is a
   larger prize than the frame rate.

**What blocks it, honestly:**

- **The CLUT is 256 entries and the 32X wants all of them.** Doom's status bar
  is drawn by the MD planes, in MD colours, and 256 + 64 does not fit. Three
  ways out, in order of how much measuring they need: (a) count the distinct
  32X CRAM indices Doom actually uses in a frame -- if it is under ~224 there
  is room to reserve the rest for MD; (b) map each MD colour to its nearest
  32X palette entry through a 64-entry table rebuilt when either CRAM changes;
  (c) second LTDC layer in RGB565 covering only the HUD rectangle, with colour
  keying for the background pixels -- the H7 LTDC has two layers.
- **Per-pixel 32X-vs-MD priority disappears** in any hardware-scanout scheme.
  It only matters where the MD layer is non-background, which for Doom's 3D
  view is nowhere -- the same fact the blank-row work established.
- **Only packed-pixel mode qualifies.** 32X direct-colour (RGB555) and
  run-length modes still need the software path, so this is a mode-selected
  fast path, not a replacement. It is chosen off the 32X VDP mode register,
  so it stays game-agnostic.
- **`lcd_setup_framebuffers` is not free to call**: it memsets both buffers,
  reconfigures the MPU and waits for a vertical-blanking reload. A per-scene
  decision, not a per-frame one.

Nothing about this is speculative plumbing -- the mode, the CLUT upload and
the bonus pool all exist and are already used by another core.

**Measured, and it closes the axis for Doom.** `RIG_PP_CENSUS` (picodrive
`ee3b3417`) counts both facts over a whole run, because a CLUT and a layer
rectangle have to serve every frame of a scene, not one:

    distinct 32X packed-pixel indices used: 256/256
    MD non-background: lines 80..223, x 0..319, 10.86% of pixels

Both answers are the unwanted one.

- **All 256 CLUT entries are spoken for**, so mitigation (a) is dead outright
  and (b) -- mapping MD colours onto nearest 32X entries -- would recolour the
  HUD rather than reproduce it.
- **The MD layer is not a status bar.** It covers the bottom 144 of 224 lines
  at full width. A second LTDC layer over that rectangle is 92 KB in RGB565,
  which is most of the 146 KB the mode was going to save; and colour keying
  can express "MD background lets the 32X through" but cannot express the
  other direction, a 32X pixel with `PXPRIO` winning over non-background MD.

So it was closed before anything was built, which is what the instrument was
for. **The instrument stays** -- another 32X title with a quieter MD layer and
a smaller palette could answer differently, and for such a title the prize is
unchanged: the compositor's inner loop plus 146 KB.

## D32XR: the official 5 MiB build boots, and the 41 fps claim does not survive

picodrive `7e04cb69` (SSF2 bank writes wired into GNW builds) is in `HEAD`,
and the official release now runs in the rig: **GATE3 PASS over 500 frames,
framebuffer alive and moving** (`/home/ubuntu/32x_roms/d32xr_official_5m_plain.32x`).

But it is not a shortcut. Per frame the rig measures

    D32XR        31.78 M host insn,  319,009 guest SH-2 insn
    retail Doom   9.88 M host insn,   94,571 guest SH-2 insn

**3.2x the host work and 3.4x the guest work.** The 41.4 drawn fps recorded
for D32XR on 2026-08-15 -- against retail Doom's 30.6 in the same session --
is arithmetically impossible against those numbers, and the handoff already
carries the discriminator for what it probably was: *60 fps with a static 3D
scene means the guest parked itself*. If D32XR is benched on the device
again, take two screenshots seconds apart and prove the picture changes
before quoting a frame rate.

---

## State

| | |
|---|---|
| Retail Doom, attract anchor | **21.79 drawn fps** (1800-frame window, 5 samples, spread 0.16) |
| Retail Doom, before today | 7.85 — the forced-draw ratio fix is worth **×2.8 on the same anchor** |
| Retail Doom, **gameplay anchor** (savestate resume) | **16.10/16.09 drawn fps** (measured 2026-08-16, screenshot-verified first-person scene) |
| Retail Doom, gameplay anchor, 08-17 tree | **15.89/15.91/15.89** (1800-frame ×3, savestate-resume arm `gpf` = 08-16 tree + the PWM-read sync restore; spread 0.02). Same anchor as above; the ~0.2 delta is the tree change |
| Retail Doom, gameplay anchor, OC A/B (08-17) | `MD32X_OC_LEVEL=1` **15.19×3** vs `=2` **17.02×3** (arms `oc1g`/`oc2g`, same committed tree, intflash byte-identical, only `32x.bin` differs) — **+12.0%, spread 0.00**. Reverses the attract-demo verdict (+0.6%); see `32X_CLOSED.md` clock-floor section. **Default raised to 2** (8653e579); default-flag build re-verified **16.94/16.95/16.95** (arm `ocdef`) |
| Retail Doom, fps axis 08-19 | **17.49 (v23) → 21.39 (five levers, arm `ahle`) → 24.02 (sound HLE, arm `hle4`) = +37% cumulative**; blank-row cache on top measured **23.66×3 = −1.46% device regression** (same-session A/B vs re-benched `hle4` 24.01×3; rig predicted −1.82% gain — sign crossed) and was **reverted** (picodrive `dc0e0b7f`+`48d3f5b6`; lesson now a rule — 32X levers need a device drawn bench before shipping, see `32X_CLOSED.md` Ledger) |
| D32XR (4 MiB bench build) | old "41.4 drawn fps" claim is **from a tree state no longer reproducible** — on 2026-08-15's tree D32XR deterministically HardFaulted at frame 59. Two bugs fixed 2026-08-16 (below); now boots and renders, but dies to `Z_Malloc: failed on 496` (open) |
| Real gameplay speed | retail Doom measured (16.1); D32XR pending its Z_Malloc fix |
| Emulated speed | ~36% of a 60 Hz machine (retail Doom attract). The console still plays in slow motion |
| Branch | `testbed`, pushed through `be6dc79b`. Submodules `external/sm` @ `5bc1605`, `external/picodrive` @ `c06b334e` (gnw-port), both on remotes |
| Arms built | `/tmp/gnw_arms/oc1`, `/tmp/gnw_arms/oc2` (cold-boot autoboot, **not** savestate-resume); `/tmp/gnw_arms/{gp,gpr,gpf,gp2,gp3,oc1g,oc2g}` (savestate-resume, 08-15→08-17 campaigns) |
| Worktree | `exp/32x-oc` — a scratch worktree used to build away from a checkout whose `external/sm` was mid-surgery |

## What changed today

**D32XR runs on the console.** Eight hours went into "which of our deltas broke
it" and the answer is none: it wedges only in `tools/m7_qemu_rig`. The screen was
verified, not inferred — see `tools/gnw_probe/screenshot.sh`. Details and the
withdrawn suspect list are in `32X_CLOSED.md` §0b.

> **2026-08-16 correction: the "wedges only in the rig" conclusion above was
> wrong.** On the then-current tree D32XR froze on the *device* too, 3/3 boots,
> at frame 59 — the same fault the rig showed. Two fork bugs were found and
> fixed (picodrive gnw-port `c06b334e`):
>
> 1. **STRD HardFault at first render.** `do_line_pp`'s solid-run blast wrote
>    two adjacent `u32` stores of the same register; gcc fused them into `STRD`,
>    and `DrawLineDest` can be 2 mod 4 — a faulting 64-bit access on Cortex-M7
>    (and QEMU's M33). Same class as Super Metroid's `ClearBackdrop`. Fix:
>    align the destination pixel first, then u32-blast.
> 2. **PWM poll-detect freeze.** A fork-added `p32x_sh2_poll_detect` on PWM
>    register reads, but PWM writes never fire `p32x_sh2_poll_event` — so the
>    slave SH-2, polling PWM CH3 during init, was CPOLL-frozen with no possible
>    wake (its `sh2irq_mask` is CMD-only). Fix: restore the upstream plain read.
>
> Device-verified after both fixes: D32XR boots and renders (69k/76.8k nonzero
> px, both SH-2s alive and advancing); retail Doom regression check on the same
> arm: 17/17 emu/drawn fps (baseline 16.1). The QEMU rig runs D32XR 200 frames
> GATE3 PASS — same rig, same ROM, now clean.

**The clock floor is not a lever — on the attract demo.** The paragraph below
was written from attract numbers; the 08-17 gameplay A/B reversed it: **+12.0%**
(15.19×3 → 17.02×3). Closed with numbers in `32X_CLOSED.md` §"clock floor".
The default is now **2** (8653e579, 340 MHz) — re-verified through a
no-flags build at 16.94/16.95/16.95.

**The 10–25 min unattended wedge is the game parking its own 68K.** Long
unattended soaks after a clean bench end in a state that *looks* like a
deadlock: 3D scene frozen, counters still pumping ~60 fps, msh2 pinned in the
SDRAM "flow" poll, ssh2 in its `bra-self`. It is not a lost wakeup — sampled
30×2 s in that state, the emulated **m68k PC is pinned at `0x8808a8`, a
`bra-self` in the ROM's own park block** (`move #$2700,sr`, interrupts off;
ROM dump + capstone, mirror at `0x880000`). The 68K vector table routes every
exception to a *different* park block (`0x8808aa` → parks at `0x8808c0`), so
the observed address is a **called** clean-shutdown, not a crash handler.
Downstream symptoms (SH-2 waits, cheap 60 fps frames) are consequences, not
causes. Not a picodrive bug, not clock-related (identical state at 312 and
340 MHz), not the PWM sync restore. Discriminator for future soaks:
**60 fps + static 3D + m68k PC pinned on a `bra-self` = the guest parked
itself — reset before measuring.**

## The anchor — the most important open item

**Every 32X fps this project has published, today's included, is the attract
demo behind the title menu.** That was confirmed by photographing the
measurement window mid-bench. It is real 3D rendering, not a still, so the
numbers are not fake — but they are not gameplay either, and the gap is large:
the same ROM measured 18.33 on 2026-08-14 against a heavier window.

There is already a gameplay savestate on the card, an early-game scene:

```
/data/32x/둠 (Doom).32x-0.sav      (+ -0.raw, its preview)
```

Use it. Build with `GNW_AUTOBOOT_STATE=1 GNW_AUTOBOOT_SLOT=0` and autoboot
**`둠 (Doom).32x`** — the savestate path is keyed by ROM filename, so the
`doom.32x` pushed today has no save and will start cold. Then re-run the OC A/B
against it: a heavier load weighs core clock against OSPI differently, and that
is the one thing that could overturn the result above.

## The queue

0. **D32XR `Z_Malloc` — ROOT CAUSE FOUND AND FIXED (2026-08-18, third rewrite;
   picodrive `7e04cb69`).** The official 5 MiB release now boots to the DOOM
   title screen in the rig. The 4 MiB bench cut still dies on 496 by a separate
   mechanism, see the tail of this entry.

   **The 5 MiB chain, every link observed on the rig** (`-DRIG_LM_TRACE` +
   `-DRIG_WALK_TRACE`, f49, logs `/tmp/opencode/lm5.log`, `bnkw1.log`):

   1. The SH-2 lump cache is a sliding-window mapper. On a miss its callback
      (`0x06006bd0`) asks the 68K over comm0 to program the cart bank window:
      `comm0 = 0x1600 | bank<<3 | slot` — observed `0x163E` (slot 6, bank 7).
   2. The 68K command handler (blob `$ff103c`, disassembled from ROM file
      0x4580+) does `move.b #bank, $a130f0 + 2*slot + 1` → `$a130fd = 7`.
      The write was *observed reaching the emulator* — `[bnkw] a=00a130fd d=07` —
      and `carthw_ssf2_banks` stayed `00..07` identity anyway.
   3. Because the bank register write was dropped, every windowed pointer read
      one 512 KiB bank low: the cache returned `0x2233ba68` (window 6) for
      TEXTURE1, the LZ decoder at `0x02018b68` read garbage (`"aa"` +
      backreference underflow past the destination), so the expanded header's
      first u32 was `0x6161` = 24929 "textures".
   4. `24929 * sizeof(texture_t=32) + 24 = 797752` → `Z_Malloc: failed on
      797752`, character for character with the device report.

   **The bug was in the emulator, not the ROM.** `PicoMemSetup32x` installs
   io write handlers under `#ifndef GNW_32X_CORE`, so GNW builds always got
   the *plain* handler, which drops every `$a130xx` write except f1. The fix
   (`7e04cb69`): compile `PicoWrite8/16_32x_on_io_ssf2` in GNW builds too
   (`carthw_ssf2_write8/16` live in `carthw.c`, compiled unconditionally) and
   route to them when `carthw_ssf2_active`. Verified: GATE3 PASS at 320/500/
   4000 frames, avg sh2 332230 insn/frame (was 0 after death), f3999
   framebuffer rendered to PNG = DOOM title screen with menu.

   Note the game never touches the SSF2 bank registers *directly* — an early
   hypothesis ("game programs banks, GNW drops them") was rejected on exactly
   that observation, then restored in the correct form: the game programs them
   *through the 68K comm proxy*, which the plain handler also starves. A
   second proxy path (opcode 0x01, `$a130f1` strobe 3/2 + a byte read at
   `$200000+2*comm1`) answers 0 — that window is still unmapped for 68K reads,
   but boot proceeds past it; it is not the killer.

   **The 4 MiB bench cut (`failed on 496`) is a different death.** Death-stack
   (`RIG_DEATH_STACK=450`, `/tmp/opencode/ds4.log`): the failing allocation is
   a 472-byte lump cache request (`496 = 472 + 24`) from
   `W_CacheLumpNum` (`0x0201edd0`) — plain zone exhaustion, not a count×32
   path. This build is on the ≤4 MiB route (wadbase 0x02036000, every lump
   pointer under the direct-return threshold), so banking is not involved and
   the fix above does not change it. **What filled the zone is not traced —
   uninvestigated, not excluded.**

   *Corrections this cost, kept honest:* the SRAM A/B of the superseded entry
   below "passed" on lying gates; read correctly it says death was identical
   under zero/FF/random SRAM — SRAM is not an input, which the root cause now
   explains (the failure was a mapper wiring gap). The instrument lesson
   stands: a checksum cannot read.

0b. *(superseded verdict, forensics retained)* **`Z_Malloc: failed on 496` —
   device-only; NOT reproducible in the rig.** The line above this
   entry used to say "Repro is deterministic offline: the QEMU rig hits the
   identical screen in 200 frames." **That claim no longer holds and it is the
   most important artifact of the investigation.** What was actually measured,
   separated from what is guessed:

   *Measured (current tree + picodrive 190f6329, ROM md5 110d2229 — byte-equal
   to the card copy pulled 08-15):*
   - The rig does not die. Real BIOS: 4000 frames, GATE3 PASS, framebuffer
     rendering, comm handshakes alive at f3999. Stub BIOS — which is what the
     device actually runs; the firmware tree has zero `p32x_bios` references,
     so `p32x_bios_m/s == NULL` takes the stub path — 4000 frames GATE3 PASS.
   - SRAM variants all PASS too: zeroed baseline, 0xff-filled, random-filled
     (2000 frames each). The game writes nothing to SRAM during boot+title,
     ignores 0xff, but *does* modify 123 bytes of random-filled SRAM — a
     non-zero SRAM is parsed and rewritten by some init path.
   - The `sh2 == 0` seen in old 200-frame runs is a boot-latency artifact, not
     death: SH-2 BIOS copy/checksum occupies f4–f169 (~167k insn/frame) while
     the 68K waits on comm4 (checksum report); handshake completes at f170;
     the MEGASD detect delay loop (~1.5M 68K cycles) runs f181+; game init
     follows. Short windows catch the quiet middle of a long boot and look
     wedged. (This also answers most of old item 4.)
   - What the error *is* (from the ported source, `z_zone.c`): 496 = ~488-byte
     request + 8-byte header, 4-aligned, failing inside the main zone. The zone
     is a compile-time fixed static array in the SH-2 program BSS
     (`BASE_ZONE_SIZE 0x33000` = 208,896 B) — identical on device and rig, so a
     device failure is an allocation-*pattern* divergence during init, not a
     smaller zone.
   - The death screen's own mechanism (from rig forensics of the boot
     sequence): the SH-2 boot program parks polling comm0=="M_OK" (CPOLL). If
     the game's 68K error path resets the SH-2, the BIOS reboot re-copies SDRAM
     and wipes the evidence — the 08-16 device SDRAM dump is post-mortem (no
     zone, no error state; the old ZONEID scan was checking for tags a custom
     allocator never writes).

   *Not known / guessed, deliberately labelled:* why the device diverges. The
   old deterministic repro came from the `exp/32x-d32xr` worktree (3677a674)
   plus throwaway probe builds, not this tree; what changed in between includes
   the D32XR STRD/PWM fixes (c06b334e, 190f6329). **Not bisected** — the old
   worktree is gone, so a bisect would have to recreate it. Remaining suspects:
   real saved-game SRAM on the card, timing, device-only codegen.

   *Leads cancelled, with reasons:* SDRAM-fastpath A/B (the rig never dies, so
   there is nothing to A/B); zone-size dump (the zone is a fixed BSS array, not
   runtime-probed — same answer on both sides).

   **What D32XR actually puts on screen in the rig (measured 2026-08-18, 4 MiB
   bench cut, current tree, 450 frames, `-DRIG_TRACE_CKS`): three static images
   and nothing else.**

   | frames | checksum | what it is |
   |---|---|---|
   | 169 | `00000000` | blank |
   | 75 | `596e0000` | still essentially black (2 colours, dumped) |
   | 206 | `2064b511` | `Z_Malloc: failed on 496` |

   It never draws a game frame. Not a title, not a menu, not a demo — the
   framebuffer takes three values in 450 frames and the last one is the
   allocator error.

   ⚠️ **This does not settle what the 41.4 fps measured, and the two sides
   disagree.** The device log for the run after the STRD/PWM fixes records
   *"boots and renders, 69k/76.8k nonzero px"* — 90% of the screen carrying
   content, which is not a line of white text on black. So on hardware D32XR was
   showing something substantial while the rig shows an allocator failure. One of
   these is not the state the other is in.

   The reason nobody can say which is that **no screenshot was taken at the
   time**. `tools/gnw_probe/screenshot.sh` exists now and did not then. Until a
   device capture of D32XR exists, treat 41.40 drawn fps as a number of unknown
   subject: `drawn` counts LCD flips, and a static screen flips too.

   ⛔ *Do not try to bisect this against `exp/32x-d32xr` @ `3677a674`.* That
   branch still exists and its submodule pin (`external/picodrive 883010c5`) is
   fetchable, so it looks like the known-good end of a regression — the tree that
   measured 41.52/41.47/41.40 drawn fps. It is not. Checked out into a detached
   worktree and run 2026-08-18: the 4 MiB bench build gives a **blank**
   framebuffer, `avg sh2=401`, GATE3 FAIL, and the official 5 MiB release the
   same. It renders nothing at all, because `883010c5` predates the STRD and PWM
   fixes (`c06b334e`) that are what made D32XR boot in the first place.

   The current tree gets strictly further: it renders, then stops on the
   allocator message. **There is no known-good rig state to bisect against**, and
   the 41.4 figure was a device measurement whose on-screen content was never
   captured — worth re-reading in that light before treating it as a target.

   *Device-owner verification, in this order (needs hardware):*
   a. Check the card for a D32XR `.sram` (the ROM header declares SRAM at
      0x200001–0x207fff). Delete it, cold-boot. The timeline fits: 08-15 20:48
      the title screen was reached (first boot, SRAM empty) — every later boot
      failed. A real saved SRAM is the one input the rig never saw.
   b. Re-verify on current HEAD — the failing observations predate 190f6329.
   c. If it still reproduces: dump SDRAM *at* the failure moment, before the
      SH-2 reboot wipes it (SWD halt + mdw, not a post-reboot dump).

   *Probe persistence:* the rig probes are now in-tree —
   `tools/m7_qemu_rig/run_32x.sh` + `RIG_SH2_WATCH` (68K PC track + SH-2
   SDRAM→BIOS edge), `RIG_STRPAGE` (68K page-0x8a string-reader trap),
   `RIG_SDRAM_SCAN` (post-death SDRAM scan), `RIG_SRAM_FILL`
   (preload/fill/dump; note QEMU semihosting passes no env vars and SYS_OPEN
   returns -1 — the knobs are compile-time macros, see HARNESSES.md),
   `RIG_DEATH_STACK` (sh2 regs/stack + SDRAM windows + 64 KiB 68K RAM dump,
   via `cart.c rig_pico_ram()`), `RIG_LM_TRACE` (comm-protocol writes from
   both sides) and `RIG_WALK_TRACE` (directory walk / cache-pointer /
   decoder-read / bank-register taps) — the last two and the death stack are
   what ran this entry's forensics (picodrive `a39ce414`, super `1ec73241`).

1. **Gameplay-anchored numbers for everything.** Retail Doom **done** —
   16.10/16.09 (2026-08-16), 15.89/15.91/15.89 on the 08-17 tree, and the OC
   A/B **done 2026-08-17**: L1 15.19×3 vs L2 17.02×3 = **+12.0%** — the clock
   floor *is* a lever in gameplay (see `32X_CLOSED.md`); whether to raise the
   default is a stability/battery call, not a measurement one. Remaining:
   D32XR (blocked on item 0).
2. **Device PC profile — DONE 2026-08-17** (gameplay anchor, savestate-resume
   arm, 2000 host-PC + 2000 guest-PC samples). Host: `sh2_execute_interpreter`
   dispatch **49.4%** of samples, SH-2 family ~60.8% total — July's "half the
   frame is SH-2 dispatch" holds in gameplay. Guest: msh2 sits 66% in three
   loops — 36.5% `R_DrawColumn` (texture column render, real work), 23.4% an
   SDRAM flag poll at `0x06001170` (Doom's 68K↔SH-2 "flow" handshake; woken by
   VINT IRQ, verified by watchpoint: nobody else writes the flag at runtime),
   6.2% software 32-bit division; ssh2 97% in a `bra-self` park plus a 5.5% PWM
   mono-FIFO poll. Follow-up built and rejected the same day: parking the SDRAM
   poll (see the ledger in `32X_CLOSED.md`) — detect cost on the read fastpath
   outweighs the spin it saves.
3. **A cart over 4 MiB now banks — DONE 2026-08-18, at a measured cost.** The
   only thing that had been compiled out under `GNW_32X_CORE` was
   `carthw_ssf2_startup()`, the standard large-ROM bank mapper. Everything else
   was already there: `pico/32x/memory.c` has had bank-aware SH-2 read paths and
   the `$a130xx` write handlers all along, running against two stub symbols that
   nothing ever set. So the official 5 MiB D32XR release was unrunnable for the
   want of one call.

   *Priced, as this entry used to ask.* `carthw.c` is ~7.6 KB of text+rodata,
   which sounded fatal against 1,236 B of `.overlay_md32x` headroom (the overlay
   was at 99.83%: 31,932 + 708,208 of 741,376). It is not, because the linker
   script claims only `.data` from `build/md32x/*.o` and sweeps every other
   object's text and rodata into `.xip_md32x` — external flash. **Measured cost:
   +24 B overlay, +16 B BSS, 40 bytes total.** Bank writes are I/O-rate, so XIP
   is the right home for them.

   *Measured behaviour (QEMU rig, 600 frames, D32XR 4 MiB card image vs the same
   image zero-padded to 5 MiB so the fallback fires):*

   | | 4 MiB | 5 MiB |
   |---|---|---|
   | gate | GATE3 PASS | GATE3 PASS |
   | fb hash f299 / f599 | `2064b511` | `2064b511` — identical |
   | avg host insn/frame | 6,960,598 | **8,789,308 (+26.3%)** |

   Bit-identical frames: turning the mapper on does not perturb a game that does
   not bank. The +26% is not the mapper's own work — it is
   `gnw_sh2_rom_fetch_mask = carthw_ssf2_active ? 0 : gnw_rom_map_mask`
   (`pico/32x/memory.c`). The cart-ROM opcode-fetch fast path is a plain
   `MAP_MEMORY(Pico.rom)` mirror, which stops being true the moment a bank can
   move, so it switches itself off and every fetch pays the cross-TU
   `p32x_sh2_read16` call instead. Doom's msh2 executes 100% out of cart ROM,
   ~173k fetches a frame.

   *And then the real ROM turned up.* It was on this machine all along, under
   `/tmp/opencode/wt32xp/*/rom.32x` (5,242,880 B, md5 `2a23fcf6…`), left by an
   earlier session. **The official 5 MiB D32XR release now loads and starts**,
   which it had never done here:

   ```
   00000:000: SSF2 mapper startup
   [32x-qemu] romlen=5242880 ssf2_active=1
   00002:091: 32X startup
   ```

   Two traps on the way, both worth keeping:

   - **That copy is byte-swapped and the 4 MiB card image is not.** Its header
     reads `ESAGS FS` where the card image reads `SEGA 32X`. The rig wants the
     plain form; fed the swapped one it runs the 68K at 926k insn/frame with
     `sh2=0` and a blank screen — indistinguishable from a wedge. Check the
     header before believing any 32X result: `dd bs=1 skip=256 count=48 | strings`
     should say `SEGA`, not `ESAGS`.
   - **The system field of the real cart is `SEGA SSF`, not `SEGA 32X`** — the
     SSF2 signature sits where the 32X one does on the bench build. 32X still
     enables (it comes from the game's ADEN write, not the header), but anything
     that keys off that field will disagree between the two ROMs.

   **Where it stands now — a device-only failure became an offline one.** With
   the real cart the mapper installs, 32X starts, the framebuffer is non-blank
   (`c8b3777c`) — and it never changes again, from f99 through f1999, while the
   SH-2s run ~1.4–2.6k insn/frame against the bench build's 61k. They are alive
   and parked. The `Z_Malloc` string trap fires zero times
   (`RIG_STRPAGE_ADDR=0x8a4c9c` — the string moved from `0x8adbdc` in the bench
   cut, and a trap pinned to the old address reports "0 hits" while watching
   nothing, so it is a knob now).

   That is a different failure from the one item 0 chased, it is deterministic,
   and it needs no device. **Next session starts here, not on hardware.** First
   questions: does the SH-2 see the banked window correctly (`memory.c:1536+`
   claims to handle it), and what is it polling when it parks?

   **Open, and the obvious next lever:** make the fetch fast path bank-aware
   rather than off — `Pico.rom + (carthw_ssf2_banks[(a >> 19) & 7] << 19) +
   (a & 0x7ffff)`, which is the same arithmetic `memory.c:1615` already does on
   the slow path. That is a hot-path edit and it was NOT attempted here, because
   the real 5 MiB D32XR release is not in this tree and a synthetic pad never
   exercises a bank switch. Get the real ROM first; it is also the only way to
   learn whether >4 MiB was ever the thing keeping it from running.
4. **Rig fidelity — mostly answered 2026-08-18, remainder low priority.** The
   old "the rig wedges" observation was the boot-latency artifact described in
   item 0: 200-frame windows sit inside the quiet middle of a ~300+-frame boot
   (BIOS copy → MEGASD detect delay → init) and read as `sh2 == 0`. Run 4000
   frames and the same rig boots, renders and handshakes to the end. What is
   still unknown about fidelity is only the pacing mapping (rig icount vs
   device wall-clock), which nobody currently needs. The rig counts
   instructions; it does not decide anything. ⛔ Do not try to settle pacing by
   linking the upstream tree inside the rig — the stub chase does not terminate
   and the result is a third program.

## Where a frame actually goes (rig, 2026-08-18)

First host-side phase breakdown. Retail Doom, **attract** anchor (no savestate —
so this is the light window; treat the shares, not the absolutes, as the
guidance), 600 frames, `EXTRA_DEF=-DRIG_PHASE_PROF`:

| phase | insn/frame | share |
|---|---|---|
| msh2 (interp+bus) | 3,898,834 | **49.5%** |
| ssh2 (interp+bus) | 1,352,060 | 17.1% |
| 32x compositor | 841,787 | 10.6% |
| draw (MD VDP line) | 724,669 | 9.2% |
| m68k | 543,001 | 6.8% |
| fm / snd / z80 / pwm | 386,682 | 4.8% |
| other (sched/ev/mem) | 129,173 | 1.6% |
| **PicoFrame total** | **7,876,239** | 100% |

**The number to aim at is not in that table: `sh2 host/guest = 75.14`.** Every
guest SH-2 instruction costs seventy-five host instructions. Two thirds of the
frame is SH-2, so that ratio *is* the core's speed. For scale: getting 75 down
to 45 would take the frame from 7.88M to ~5.8M, about −27%, without touching a
single guest cycle. That is the largest lever anyone has priced here, and the
tree already has the precedent for it — the SNES core runs a hand-written
Thumb-2 65816 and SPC700 for exactly this reason.

⛔ **And do not "simplify" the fastloop pre-filter out of the dispatch.** It
costs ~7 host instructions on every guest instruction and looks exactly like the
add-a-test-to-skip-work shape this tree normally loses on. Priced by ablation
(`-DGNW_SH2_NO_FASTLOOPS`, picodrive `8023c183`): removing it takes the frame
from 7,876,239 to **27,488,194** insn — 3.5x worse — because the guest
instructions actually interpreted go from 69,878 to 434,967. The filter is
buying the loops the game lives in. Note the trap in the ratio: host/guest
*improves* from 75.1 to 57.2 when you remove it, which is a good reminder that
host-per-guest is not a figure of merit by itself — it gets better when you make
the guest do more work.

### Why idle-skip breaks the sound — mechanism, measured 2026-08-18

The old verdict was "it broke Doom's gunshot PWM SFX", found by ear on hardware.
The rig can hear now (audio hash, `dfa8678d`) and the mechanism is this:

| arm | insn/frame | framebuffer | audio hash |
|---|---|---|---|
| baseline | 7,694,031 | 9cd2510f/22ee77a6/ced1080b | `f4c01e1e` |
| idle-skip | 6,329,078 (−17.7%) | identical | **`3ac9f381`** |
| idle-skip + wake on PWM FIFO movement | 7,662,594 (−0.4%) | identical | **`f4c01e1e`** ✓ |

A parked SH-2 is only woken by an internal IRQ (`sh2_internal_irq`); `pwm.c`
contains no wake at all, and the VBlank `p32x_sh2_poll_event` excludes
`SH2_STATE_SLEEP` explicitly. Doom's **slave SH-2 does not wait on the PWM
interrupt — it busy-polls the mono FIFO count**, so once parked it never sees
the FIFO drain and never refills it. Waking it on FIFO movement restores the
baseline audio hash exactly, which confirms the mechanism.

And then the gain is gone: −17.7% becomes −0.4%, whether the wake fires on every
consumed sample or only when the FIFO empties. **ssh2's 17.1% of the frame is not
idle spin — it is the sound service loop**, running at audio rate. The spin *is*
the waiting, and skipping the waiting skips the sound.

So the old verdict stands, but for a better reason than "it broke SFX", and the
next attempt has a gate that catches it offline in one run instead of a device
round trip and an ear.

**What this does leave open:** neither existing collapse handles a poll on a
*PWM register*. The SDRAM-poll detector deliberately restricts itself to
`0x06000000` targets (an `RW()` on a sysreg/comm address would fire poll_detect
and corrupt the guest's poll state), and the bra-self path is a different shape.
A detector that understands the PWM FIFO could compute when the count will next
change and skip to exactly there — cycle-exact, so the audio hash would have to
stay `f4c01e1e`. That is the one untried shape on this axis.

⛔ **Do not re-derive the idle-skip lever from this table.** ssh2's 17.1% is
almost entirely a `bra-self` park, and switching on the existing
`gnw_sh2_idle_skip` removes it: measured here 7,876,239 → 6,511,024 insn/frame,
**−17.3%, with all three framebuffer hashes identical**. It is still closed, for
reasons recorded in `main_md32x.c` and `32X_PERFORMANCE_RESULTS.md` 측정10: on
the **device** it measured **0 fps effect** (rig instruction savings did not
translate to device cycles), its CRC32 whitelist matched one dump, and it broke
Doom's gunshot PWM SFX — which no framebuffer hash can see. A clean rig A/B with
identical frames is exactly what this lever looked like the last time too.

### What is already optimised — priced by ablation 2026-08-18, do not re-derive

Four levers were re-examined in one sitting and **all four turned out to be
already done or already closed**. Each was priced by building both arms and
checking the two binaries actually differ, because two of them first showed a
delta of exactly zero:

| lever | status | measured |
|---|---|---|
| SH-2 fastloop filter | **already on** (`sh2pico.c` defines it unconditionally) | removing it: 7.88M → 27.49M insn/frame, 3.5× worse |
| BF/S countdown collapse (`TST Rn,Rn` / `ADD #-1,Rn`) | **already implemented** | verified firing at Doom's hottest loop, `r4=0x31`, 48 of 49 iterations collapsed |
| SH-2 idle skip | ⛔ closed on the device | rig −17.3%, device 0 fps, broke Doom's PWM SFX |
| packed-pixel solid-run detector | **already on and paying** | removing it: compositor 773k → 945k insn/frame, +22% |

The two zero-deltas were the instructive part. `-DGNW_SH2_FASTLOOPS` changed
nothing because the feature was already unconditionally enabled, and a
hand-written BF/S countdown collapse changed nothing because an identical
handler already sat earlier in the same function. Both would have been reported
as "no gain" if the arms had not been md5'd first.

Ablation switches were left behind so the next person re-prices in one build
instead of deleting code to find out: `GNW_SH2_NO_FASTLOOPS`, `GNW_PP_NO_RUNDET`.

**The MD VDP renderer was the last unexamined bucket, and its ceiling is low.**
`GNW_MD_ABLATE` (picodrive `6b403f94`) kills every MD layer so the bucket
collapses to what is irreducible: draw 724,669 → 345,920 insn/frame, whole frame
7,700,620 → 7,279,779, **−5.5% for deleting the layer entirely**. Two thirds of
its cost is per-line overhead that survives having nothing to draw. And it is not
deletable: the framebuffer hashes change (f99 and f299 go blank), so Doom's MD
layer carries visible content, not merely the background mask the compositor
tests. Any real optimisation there is a fraction of 5.5%.

**Where that leaves the frame.** Retail Doom, attract, 400 frames: the
compositor is 10.0% at ~10.8 host instructions per pixel, MD VDP draw is 9.2%,
and two thirds is SH-2 executing the game. The interpreter-level levers are
spent. The MD VDP renderer has now been priced too (above), and it is
not the lever it looked like. **Everything cheap is spent.** What remains is
structural: the SH-2 interpreter itself, ~66% of the frame, where the tree's own
precedent is the SNES core's hand-written Thumb-2 65816 and SPC700. That is a
project, not an afternoon, and it should be entered with an ablation estimate of
what a faster dispatch is actually worth on the *device* — the idle-skip lesson
is that rig instruction savings do not automatically become device fps.

### Dispatch-prologue ablation — the estimate the paragraph above asked for (2026-08-18)

The interpreter's dispatch was taken apart two ways: statically, from the A0
ELF's `sh2_execute_interpreter` disassembly, and dynamically, by building
single-knob ablation arms in a throwaway lane and running the gameplay anchor
(retail Doom, savestate resume, 900 frames, workload identity verified by
identical `avg sh2 = 94,406` dispatched/frame on every valid arm; every arm
md5'd against baseline first). **The measurement builds were never committed —
that is the standing rule for ablation arms.**

*Static, per dispatched guest instruction (direct path), host instructions
around the handler body:*

| stage | insn |
|---|---|
| delay-slot check | 3 |
| pc load + ppc store | 2 |
| fetch fast path (two masks, direct load) | 11 |
| fastloop pre-filter, interleaved with state updates | 11 |
| dispatch jump (sp table: add/lsl/ldr/orr/bx) | 5 |
| handler stub (`mov r0,r4; bl opNNNN; b back`) | ~3 |
| loop tail (icount--, irq check, both loop conditions) | 11 |
| **total around the handler** | **≈52** |

Plus ~15 host instructions once per *slice* (the 16-word stack dispatch-table
copy) — dozens of times a frame, negligible. Against the measured
`host/guest = 75.14`, the handler bodies themselves are only ~23 of ~75 host
insns: **the scaffolding around each guest instruction is ~2/3 of interpreter
cost.**

*Ablation, avg host insn/frame (lower is cheaper):*

| arm | change | avg host | delta |
|---|---|---|---|
| A0 baseline (a39ce414 clean) | — | 9,802,331 | — |
| A2 dispatch table → rodata | slice copy removed, PC-relative load | 9,712,214 | **−0.92%** |
| A3 switch dispatch (replaces computed goto + stubs) | compiler jump table | 9,590,466 | **−2.16%** |
| A4 no fastloop pre-filter | filter + fastloop removed | 28,214,186 | **+187.8% (2.88×)** — dispatched inflates 94k→442k; the filter is buying exactly the loops the game lives in. Confirms `8023c183` (3.5× on attract). Keep it. |
| A5 no IRQ delivery | tail `if(0)` | 31.9M | **invalid measurement** — IRQ never delivered, workload diverges; priced nothing |
| A5b tail drops `!delay` half of irq check | `if(test_irq)` | 9,704,547 | **−1.0%**, semantics unproven (irq taken during a pending delay slot) — measured, not proposed |

**What this answers, question by question.**

1. *Where the dispatch prologue goes:* the table above — fetch fast path and
   the interleaved filter+state-update block are the two 11-insn items; the
   dispatch jump itself is only 5.
2. *What can actually be shaved today:* semantics-identical ceiling measured at
   **A3+A5b ≈ −3.2% host cost**, of which A3 alone (switch beats the
   hand-rolled stack-table computed goto — GCC's rodata jump table is cheaper
   than `add/lsl/ldr/orr/bx` off sp) is −2.16% and trivially shippable. A2's
   −0.92% is subsumed by A3.
3. *Device translation:* SH-2 family is 66.6% of the device frame; a ~3%
   whole-frame rig saving is ~+0.5 fps at 16.95. Real, cheap, not a game
   changer. The structural target is different: if the ~52-insn scaffolding is
   ~60% of interpreter time ≈ **~40% of the whole frame**, then a Thumb-2
   dispatch core that *halves* the scaffolding is worth ~20% of the frame —
   16.95 → ~21 fps. That last number is an **estimate** (static counts over
   the measured 75.14 host/guest), not an ablation; the ablated part is only
   the −3.2% ceiling of re-arranging the existing C.

Lane/protocol detail for whoever re-runs: lane `pd2` = clone of picodrive
`a39ce414`, only `cpu/sh2/mame/sh2pico.c` edited per arm, restored with
`git checkout` after; arms md5'd (`8e5daf6d` baseline, `57e00101` A2, …);
`avg sh2` equality is the workload-identity gate — A5 shows why (its game
broke, and its 3.26× "cost" was the breakage, not the tail check).

⚠️ And do not instrument `FinalizeLine32xRGB555` looking for the compositor. Its
own comment says "almost never used (Wiz and menu bg gen only)"; Doom composites
through `PicoDraw32xLayer`, in packed-pixel mode (`Mx=1`, H40, 224 lines,
`Pico32xDrawMode=1`, no scan hooks) — confirmed by instrumenting it, after a
probe in the other function printed nothing at all.

## How to measure this core without wasting a day

Four things cost real time on 2026-08-15. All four are cheap to avoid.

- **1800-frame windows, never 900.** At 900 the same arm read 26.84 / 26.89 /
  29.18 — a spread the size of the effect under test. At 1800 it reads
  21.77–21.93. Short windows sit on the light part of the demo and read
  optimistically.
- **Hash the card's `/cores/32x.bin` against the arm's.** Every `MD32X_C_DEFS`
  knob leaves both arms' intflash byte-identical, so `drawn_ab.sh`'s flash-side
  check passes for either one. Its card-side check was silently skipping until
  today (`arm32x.sh` wrote the core to the arm root, `drawn_ab.sh` looked under
  `cores/`). It printed `SKIPPED` in every run and went unread for a whole
  bench: **grep the output for it.**
- **Photograph the measurement window.** `screenshot.sh --live` takes ten
  seconds and settles "which scene was that" permanently. Half a day of numbers
  was untrustworthy for want of one image.
- **Reset before measuring after any idle period.** An unattended device that
  has been sitting in Doom for 10–25 minutes may have parked its own 68K (see
  the wedge note above) — the counter then reports a meaningless ~60 fps. One
  `reset run` (or any bench script's built-in reset) restores the real anchor.
- **Build in a worktree.** `build/` is shared across every session in a
  checkout, and `external/` can be mid-edit by someone else. Two builds died
  that way today — one on a truncated object left by a killed build, one on
  another session's in-flight work.
