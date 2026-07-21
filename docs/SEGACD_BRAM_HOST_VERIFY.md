# Sega CD BRAM footer host verification

Date: 2026-07-20

## Result

The formatted-BRAM footer does not change execution in the current host harness.
The zero-filled and PicoDrive-formatted variants follow the same sub-68K state
transitions, including the game coroutine state at PRG-RAM `$97e8`, both on the
natural boot path and on the harness's forced game/IP path.

The direct reason is that the current sub-68K memory map has no mapping for the
internal BRAM range `$FE0000-$FEFFFF`.  Consequently the CPU cannot observe the
contents initialized by `segacd_bram_load()`.

## Method

`tools/segacd_harness/boot_test.c` loads a deliberately missing BRAM file after
`segacd_init()`.  One binary then selects either:

- formatted: the 64-byte PicoDrive `formatted_bram` footer at the end of BRAM;
- zero: `SCD_BRAM_ZERO=1`, which overwrites the same BRAM allocation with zeroes.

START is pulsed for 8 frames beginning at frame 900 and every 240 frames after
that.  The trace records mode changes, `$97e8..$97ef`, final CPU state, and
sub-68K BRAM accesses.  The normalized trace excludes the explicit A/B variant
label.

Test image: Detonator Orgun, US BIOS.  Harness binary: `build_hist.sh` output.

## Measurements

### Natural boot, 2,000 frames

Normalized trace SHA-256:

```
formatted  b8c5361cb18ef8f2d4bca16c6ba43e7998bb269230424117b869ed41071a923a
zero       b8c5361cb18ef8f2d4bca16c6ba43e7998bb269230424117b869ed41071a923a
```

Both variants reach mode `$0008` and then remain at the same pre-game point:

- MAIN PC: `$000a1e`
- SUB PC: alternates around `$006132/$006136`
- `$97e8..$97eb`: `$000800ff`

This checkout does not naturally reach the title screen, so it cannot reproduce
the exact device-side `Press START` symptom.  It does establish that formatting
BRAM does not change the earlier host execution path.

### Forced game/IP path, 3,000 frames

With the existing harness-only `SCD_FAST_BOOT=1` path, both variants enter mode
`$001c` at frame 727 and mode `$000c` at frame 900.  The `$97e8` transitions are
identical:

```
f727  0008ff00025e0000
f728  00080000025e0000
f753  000800ff026bff00
f901  000c000002ff0000
f902  000cff0002ff0000
```

Normalized trace SHA-256:

```
formatted  f349998c591cfddfe64e023d50f8c23e1d7f1e3658c82e26f3fbdc686d6edda5
zero       f349998c591cfddfe64e023d50f8c23e1d7f1e3658c82e26f3fbdc686d6edda5
```

### BRAM visibility

Across the 3,000-frame forced-game run:

```
sub reads=0 writes=0
map[0xfe].base=NULL
map[0xfe].read8=NULL  read16=NULL
map[0xfe].write8=NULL write16=NULL
```

`segacd_sub_build_memory_map()` maps PRG RAM, Word RAM, gate-array/PCM space,
and BIOS, but not sub-68K page `$FE`.  This makes the formatted footer inert in
the tested core.

## Conclusion

The footer initialization is correct filesystem data, but by itself it cannot
resolve the current `Press START` failure because the sub-68K has no path to
read it.  The next prerequisite is a correct `$FE0000` internal-BRAM mapping;
after that, repeat this same zero/formatted A/B and device symptom test.

No production fix was committed in this verification.  The BRAM formatter and
the probes remain working-tree-only experiments.

## Follow-up: `$FE0000` mapping present

Commit `10db9b1c`'s PicoDrive-compatible `$FE0000` handlers were applied to the
same harness and the A/B was repeated. The map entry is now live (`read8`,
`read16`, `write8`, and `write16` are all non-NULL), but Detonator Orgun still
does not access that page on either tested path:

```
natural, 2,000 frames:    SUB BRAM reads=0 writes=0
forced game, 3,000 frames: SUB BRAM reads=0 writes=0
```

Normalized traces remain identical between formatted and zero BRAM:

```
natural formatted/zero: 576f347add0f00af89913eba7a174bbdfd47c861408b8723bf80ac582943fb91
forced  formatted/zero: ec365ab80baecd55e88ff7a2af402fe30a973e3c5191057d73c90750923775ec
```

Thus the map is implemented, but the current execution never reaches code that
queries BRAM. Formatting remains inert for this symptom.

An input control run does show that host-side START is accepted after the
harness's forced IP entry. With START at frame 900, MAIN mode changes
`$001c->$000c` and `$97e8` changes through `000c0000` to `000cff00`; without
START, mode remains `$001c` through frame 1,200. The failure is after input,
not in input delivery.

The strongest next lead is the COMREG rendezvous. After START, MAIN spends most
of its sampled time in `$1358-$1388`, with `$FDDD=1`, waiting for `$A1200F`
bit 7 to clear. SUB's last observed comm write at frame 1,168 is PC `$60c4`,
value `$80`, and `$A1200F` is still `$80` at the stop. Word-RAM register 3 is
not static: both sides perform hundreds of `04<->05` and `09/0a` transitions,
so a completely missing `$A12003` handshake is less consistent with the trace.
Level-2 interrupts are also delivered and the state machine advances, leaving
CDD IRQ timing as a possible but less direct third lead.

## Follow-up: COMREG rendezvous

The COMREG backing-store hypothesis is negative. The actual byte paths (not
comments) converge on the same storage:

- SUB byte write `$FF800E/F` in `sub_ff_write8()` stores
  `SCD.s68k_regs[0x0f]`.
- MAIN byte read `$A1200F` in `main_ga_read8()` returns
  `SCD.s68k_regs[0x0f]`.
- Word handlers merely compose/decompose those byte handlers, so the observed
  byte access has no alternate shadow.

The local PicoDrive reference has the same software-owned protocol.
`pd_cd/memory.c:s68k_reg_write8()` aliases SUB `$0e` to `$0f` and writes the
shared byte; `m68k_reg_read16()` reads it directly. Its `write_comm` block only
wakes a polling CPU. It does not clear or transform bit 7. The local
`pd_cd/gfx.c` touches graphics registers `$58+`, not COMREG `$0e/$0f`.
Therefore gate-array hardware is not expected to auto-clear this bit.

To isolate execution after the first observed `$60bc` write, the harness was
run to frame 1,400 with START at frame 900 and the PC histogram cleared at
frame 1,169. Over the resulting 231 frames, SUB was not trapped in a wait:

```
SUB sampled instructions: 696,297
$18c04/$18c06/$18c08 PCM copy loop: 192,402 each (82.89% combined)
longest repeated read: 112 reads of $005ea4 at PC $0005ee
$0097eb reads: 0
$FF800F reads: 0
```

`$5e8/$5ee` is the BIOS IFL2-doorbell wait and its bit is cleared by the
level-2 ISR at `$5f2`; the bounded 112-read maximum shows that this wait is
being released, not that SUB is permanently parked there.

A second run armed at frame 905 counted every subsequent SUB COMREG write:

```
game set-80 PC $0060bc: 15 executions
BIOS init clear PC $0002a6: 0 executions
BIOS bit-7 clear PC $0004da: 0 executions
all 15 observed writes: value $80 (handler reports PC $0060c4 after advance)
MAIN COMREG writes after frame 905: 0
final regs[0x0e]=80, regs[0x0f]=80, $FDDD=01
```

Thus the apparent "last write" at frame 1,168 was just the last periodic
`$60bc` execution before that shorter run ended. SUB periodically executes
the service workload and reasserts `$80`; it never executes a clear writer.
MAIN has likewise left its request byte (`regs[0x0e]`) at `$80`. This is a
software protocol/call-completion failure, not a split COMREG store or missing
hardware clear side effect.

Of the remaining nominated leads, Word-RAM `$A12003` synchronization is next.
COMREG itself is ruled out, and the bounded/released `$5ea4` doorbell wait
makes a wholly missing level-2 delivery less consistent with this trace.

## Follow-up: Word-RAM ownership and the `$D01C` dead link

The reg3 ownership state machine is not the failing link. A frame-local access
trace shows the intended 1M bank handoff:

```
f742 SUB  r3=05  $0C0401 W8  $20  map base=word_ram+0
f755 MAIN r3=04  $200400 R16       map base=word_ram+0
```

SUB owns physical bank 0 while bit 0 is 1, then MAIN sees the same bank after
SUB toggles bit 0 back to 0. There is no interval in this transfer where both
CPUs map different physical banks while believing they received the handoff.
The persistent `dmna_ret_2m` shadow is `$01` throughout this 1M exchange, as in
PicoDrive; it is not suppressing a completion response.

The actual bug is in the type of MAIN memory-map entry. Commit `b645e18a`
updated only `.base` for pages `$20-$23`. Those pages already contain Genesis
ROM/EEPROM callbacks installed by `gwenesis_bus_init_memory_map()`. Musashi
dispatches a non-NULL callback before using `.base`, so the apparently correct
Word-RAM pointer was ignored. PicoDrive's `cpu68k_map_all_ram()` replaces the
whole mapping, not only the backing pointer.

Observed A/B at the exact transfer:

```
                         callbacks retained       callbacks cleared
physical word_ram+$400   20 00 00 00               20 00 00 00
MAIN read $200400        0000                      0020
MAIN write $200400=0000  ignored                   stored
copy to $FFD01C          never receives status     writes 0020
BIOS $2e2e consumer      not entered for status    reads 0020 and processes it
```

The working-tree fix makes every direct Word-RAM mapping set `.base` and clear
all four callbacks (`read8/read16/write8/write16`) together, for both 2M and 1M
views. The host build passes, and 2,000-frame natural plus 3,000-frame forced-IP
runs complete without a crash.

This also proves that the final `$D01C=0` observation was a result, not the
cause: with the fix, frame 755 copies `$0020` to `$FFD01C`, `$2e2e` consumes
the status in the same frame, and the following write-back clears
`$200400/$D01C`. An end-of-run snapshot therefore legitimately shows zero.

The fix does not resolve the post-START stall. The forced-IP run still changes
mode `$1c->$0c` at frame 900 and reaches `$97e8=000cff00`, then ends with both
COMREG flag bytes `$80`. Word-RAM mapping was a real independent defect, while
the remaining request/ack clear-path prerequisite lies elsewhere.
